
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <fadec.h>


#ifdef __GNUC__
#define LIKELY(x) __builtin_expect((x), 1)
#define UNLIKELY(x) __builtin_expect((x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

// Defines FD_TABLE_OFFSET_32 and FD_TABLE_OFFSET_64, if available
#define FD_DECODE_TABLE_DEFINES
#include <fadec-decode-private.inc>
#undef FD_DECODE_TABLE_DEFINES

enum DecodeMode {
    DECODE_64 = 0,
    DECODE_32 = 1,
};

typedef enum DecodeMode DecodeMode;

#define ENTRY_NONE 0
#define ENTRY_INSTR 1
#define ENTRY_TABLE256 2
#define ENTRY_TABLE16 3
#define ENTRY_TABLE8E 4
#define ENTRY_TABLE_PREFIX 5
#define ENTRY_TABLE_VEX 6
#define ENTRY_TABLE_ROOT 8
#define ENTRY_MASK 7

static unsigned
table_walk(unsigned cur_idx, unsigned entry_idx, unsigned* out_kind) {
    static _Alignas(16) const uint16_t _decode_table[] = {
#define FD_DECODE_TABLE_DATA
#include <fadec-decode-private.inc>
#undef FD_DECODE_TABLE_DATA
    };
    unsigned entry = _decode_table[cur_idx + entry_idx];
    *out_kind = entry & ENTRY_MASK;
    return (entry & ~ENTRY_MASK) >> 1;
}

#define LOAD_LE_1(buf) ((uint64_t) *(uint8_t*) (buf))
#define LOAD_LE_2(buf) (LOAD_LE_1(buf) | LOAD_LE_1((uint8_t*) (buf) + 1)<<8)
#define LOAD_LE_3(buf) (LOAD_LE_2(buf) | LOAD_LE_1((uint8_t*) (buf) + 2)<<16)
#define LOAD_LE_4(buf) (LOAD_LE_2(buf) | LOAD_LE_2((uint8_t*) (buf) + 2)<<16)
#define LOAD_LE_8(buf) (LOAD_LE_4(buf) | LOAD_LE_4((uint8_t*) (buf) + 4)<<32)

enum
{
    PREFIX_REXB = 0x01,
    PREFIX_REXX = 0x02,
    PREFIX_REXR = 0x04,
    PREFIX_REXW = 0x08,
    PREFIX_REX = 0x40,
    PREFIX_REXRR = 0x10,
};

struct InstrDesc
{
    uint16_t type;
    uint16_t operand_indices;
    uint16_t operand_sizes;
    uint16_t reg_types;
};

#define DESC_HAS_MODRM(desc) (((desc)->operand_indices & (3 << 0)) != 0)
#define DESC_MODRM_IDX(desc) ((((desc)->operand_indices >> 0) & 3) ^ 3)
#define DESC_HAS_MODREG(desc) (((desc)->operand_indices & (3 << 2)) != 0)
#define DESC_MODREG_IDX(desc) ((((desc)->operand_indices >> 2) & 3) ^ 3)
#define DESC_HAS_VEXREG(desc) (((desc)->operand_indices & (3 << 4)) != 0)
#define DESC_VEXREG_IDX(desc) ((((desc)->operand_indices >> 4) & 3) ^ 3)
#define DESC_IMM_CONTROL(desc) (((desc)->operand_indices >> 12) & 0x7)
#define DESC_IMM_IDX(desc) ((((desc)->operand_indices >> 6) & 3) ^ 3)
#define DESC_EVEX_BCST(desc) (((desc)->operand_indices >> 8) & 1)
#define DESC_EVEX_MASK(desc) (((desc)->operand_indices >> 9) & 1)
#define DESC_ZEROREG_VAL(desc) (((desc)->operand_indices >> 10) & 1)
#define DESC_LOCK(desc) (((desc)->operand_indices >> 11) & 1)
#define DESC_VSIB(desc) (((desc)->operand_indices >> 15) & 1)
#define DESC_OPSIZE(desc) (((desc)->reg_types >> 11) & 7)
#define DESC_SIZE_FIX1(desc) (((desc)->operand_sizes >> 10) & 7)
#define DESC_SIZE_FIX2(desc) (((desc)->operand_sizes >> 13) & 3)
#define DESC_INSTR_WIDTH(desc) (((desc)->operand_sizes >> 15) & 1)
#define DESC_MODRM(desc) (((desc)->reg_types >> 14) & 1)
#define DESC_IGN66(desc) (((desc)->reg_types >> 15) & 1)
#define DESC_EVEX_SAE(desc) (((desc)->reg_types >> 8) & 1)
#define DESC_EVEX_ER(desc) (((desc)->reg_types >> 9) & 1)
#define DESC_EVEX_BCST16(desc) (((desc)->reg_types >> 10) & 1)
#define DESC_REGTY_MODRM(desc) (((desc)->reg_types >> 0) & 7)
#define DESC_REGTY_MODREG(desc) (((desc)->reg_types >> 3) & 7)
#define DESC_REGTY_VEXREG(desc) (((desc)->reg_types >> 6) & 3)

int
fd_decode(const uint8_t* buffer, size_t len_sz, int mode_int, uintptr_t address,
          FdInstr* instr)
{
    int len = len_sz > 15 ? 15 : len_sz;

    // Ensure that we can actually handle the decode request
    DecodeMode mode;
    unsigned table_idx;
    unsigned kind = ENTRY_TABLE_ROOT;
    switch (mode_int)
    {
#if defined(FD_TABLE_OFFSET_32)
    case 32: table_idx = FD_TABLE_OFFSET_32; mode = DECODE_32; break;
#endif
#if defined(FD_TABLE_OFFSET_64)
    case 64: table_idx = FD_TABLE_OFFSET_64; mode = DECODE_64; break;
#endif
    default: return FD_ERR_INTERNAL;
    }

    int off = 0;
    uint8_t vex_operand = 0;

    unsigned prefix_rep = 0;
    bool prefix_lock = false;
    bool prefix_66 = false;
    uint8_t addr_size = mode == DECODE_64 ? 3 : 2;
    unsigned prefix_rex = 0;
    int rex_off = -1;
    unsigned vexl = 0;
    unsigned prefix_evex = 0;
    instr->segment = FD_REG_NONE;

    if (mode == DECODE_32) {
        while (LIKELY(off < len))
        {
            uint8_t prefix = buffer[off];
            switch (UNLIKELY(prefix))
            {
            default: goto prefix_end;
            // From segment overrides, the last one wins.
            case 0x26: instr->segment = FD_REG_ES; break;
            case 0x2e: instr->segment = FD_REG_CS; break;
            case 0x36: instr->segment = FD_REG_SS; break;
            case 0x3e: instr->segment = FD_REG_DS; break;
            case 0x64: instr->segment = FD_REG_FS; break;
            case 0x65: instr->segment = FD_REG_GS; break;
            case 0x66: prefix_66 = true; break;
            case 0x67: addr_size = 1; break;
            case 0xf0: prefix_lock = true; break;
            case 0xf3: prefix_rep = 2; break;
            case 0xf2: prefix_rep = 3; break;
            }
            off++;
        }
    }
    if (mode == DECODE_64) {
        while (LIKELY(off < len))
        {
            uint8_t prefix = buffer[off];
            switch (UNLIKELY(prefix))
            {
            default: goto prefix_end;
            // ES/CS/SS/DS overrides are ignored.
            case 0x26: case 0x2e: case 0x36: case 0x3e: break;
            // From segment overrides, the last one wins.
            case 0x64: instr->segment = FD_REG_FS; break;
            case 0x65: instr->segment = FD_REG_GS; break;
            case 0x66: prefix_66 = true; break;
            case 0x67: addr_size = 2; break;
            case 0xf0: prefix_lock = true; break;
            case 0xf3: prefix_rep = 2; break;
            case 0xf2: prefix_rep = 3; break;
            case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
            case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a: case 0x4b:
            case 0x4c: case 0x4d: case 0x4e: case 0x4f:
                prefix_rex = prefix;
                rex_off = off;
                break;
            }
            off++;
        }
    }

prefix_end:
    // REX prefix is only considered if it is the last prefix.
    if (rex_off != off - 1)
        prefix_rex = 0;

    if (UNLIKELY(off >= len))
        return FD_ERR_PARTIAL;

    unsigned opcode_escape = 0;
    uint8_t mandatory_prefix = 0; // without escape/VEX/EVEX, this is ignored.
    if (buffer[off] == 0x0f)
    {
        if (UNLIKELY(off + 1 >= len))
            return FD_ERR_PARTIAL;
        if (buffer[off + 1] == 0x38)
            opcode_escape = 2;
        else if (buffer[off + 1] == 0x3a)
            opcode_escape = 3;
        else
            opcode_escape = 1;
        off += opcode_escape >= 2 ? 2 : 1;

        // If there is no REP/REPNZ prefix offer 66h as mandatory prefix. If
        // there is a REP prefix, then the 66h prefix is ignored here.
        mandatory_prefix = prefix_rep ? prefix_rep : !!prefix_66;
    }
    else if (UNLIKELY((unsigned) buffer[off] - 0xc4 < 2 || buffer[off] == 0x62))
    {
        unsigned vex_prefix = buffer[off];
        // VEX (C4/C5) or EVEX (62)
        if (UNLIKELY(off + 1 >= len))
            return FD_ERR_PARTIAL;
        if (mode == DECODE_32 && (buffer[off + 1] & 0xc0) != 0xc0)
            goto skipvex;

        // VEX/EVEX + 66/F3/F2/REX will #UD.
        // Note: REX is also here only respected if it immediately precedes the
        // opcode, in this case the VEX/EVEX "prefix".
        if (prefix_66 || prefix_rep || prefix_rex)
            return FD_ERR_UD;

        uint8_t byte = buffer[off + 1];
        if (vex_prefix == 0xc5) // 2-byte VEX
        {
            opcode_escape = 1 | 4; // 4 is table index with VEX, 0f escape
            prefix_rex = byte & 0x80 ? 0 : PREFIX_REXR;
        }
        else // 3-byte VEX or EVEX
        {
            // SDM Vol 2A 2-15 (Dec. 2016): Ignored in 32-bit mode
            if (mode == DECODE_64)
                prefix_rex = byte >> 5 ^ 0x7;
            if (vex_prefix == 0x62) // EVEX
            {
                if (byte & 0x08) // Bit 3 of opcode_escape must be clear.
                    return FD_ERR_UD;
                opcode_escape = (byte & 0x07) | 8; // 8 is table index with EVEX
                _Static_assert(PREFIX_REXRR == 0x10, "wrong REXRR value");
                if (mode == DECODE_64)
                    prefix_rex |= (byte & PREFIX_REXRR) ^ PREFIX_REXRR;
            }
            else // 3-byte VEX
            {
                if (byte & 0x1c) // Bits 4:2 of opcode_escape must be clear.
                    return FD_ERR_UD;
                opcode_escape = (byte & 0x03) | 4; // 4 is table index with VEX
            }

            // Load third byte of VEX prefix
            if (UNLIKELY(off + 2 >= len))
                return FD_ERR_PARTIAL;
            byte = buffer[off + 2];
            prefix_rex |= byte & 0x80 ? PREFIX_REXW : 0;
        }

        mandatory_prefix = byte & 3;
        vex_operand = ((byte & 0x78) >> 3) ^ 0xf;

        if (vex_prefix == 0x62) // EVEX
        {
            if (!(byte & 0x04)) // Bit 10 must be 1.
                return FD_ERR_UD;
            if (UNLIKELY(off + 3 >= len))
                return FD_ERR_PARTIAL;
            byte = buffer[off + 3];
            // prefix_evex is z:L'L/RC:b:V':aaa
            vexl = (byte >> 5) & 3;
            prefix_evex = byte | 0x100; // Ensure that prefix_evex is non-zero.
            if (mode == DECODE_64) // V' causes UD in 32-bit mode
                vex_operand |= byte & 0x08 ? 0 : 0x10; // V'
            else if (!(byte & 0x08))
                return FD_ERR_UD;
            off += 4;
        }
        else // VEX
        {
            vexl = byte & 0x04 ? 1 : 0;
            off += 0xc7 - vex_prefix; // 3 for c4, 2 for c5
        }

    skipvex:;
    }

    table_idx = table_walk(table_idx, opcode_escape, &kind);
    if (kind == ENTRY_TABLE256 && LIKELY(off < len))
        table_idx = table_walk(table_idx, buffer[off++], &kind);

    // Handle mandatory prefixes (which behave like an opcode ext.).
    if (kind == ENTRY_TABLE_PREFIX)
    {
        table_idx = table_walk(table_idx, mandatory_prefix, &kind);
    }

    // Then, walk through ModR/M-encoded opcode extensions.
    if (kind == ENTRY_TABLE16 && LIKELY(off < len)) {
        unsigned isreg = (buffer[off] & 0xc0) == 0xc0 ? 8 : 0;
        table_idx = table_walk(table_idx, ((buffer[off] >> 3) & 7) | isreg, &kind);
        if (kind == ENTRY_TABLE8E)
            table_idx = table_walk(table_idx, buffer[off] & 7, &kind);
    }

    // For VEX prefix, we have to distinguish between VEX.W and VEX.L which may
    // be part of the opcode.
    if (UNLIKELY(kind == ENTRY_TABLE_VEX))
    {
        uint8_t index = 0;
        index |= prefix_rex & PREFIX_REXW ? (1 << 0) : 0;
        // When EVEX.L'L is the rounding mode, the instruction must not have
        // L'L constraints.
        index |= vexl << 1;
        table_idx = table_walk(table_idx, index, &kind);
    }

    if (UNLIKELY(kind != ENTRY_INSTR))
        return kind == 0 ? FD_ERR_UD : FD_ERR_PARTIAL;

    static _Alignas(16) const struct InstrDesc descs[] = {
#define FD_DECODE_TABLE_DESCS
#include <fadec-decode-private.inc>
#undef FD_DECODE_TABLE_DESCS
    };
    const struct InstrDesc* desc = &descs[table_idx >> 2];

    instr->type = desc->type;
    instr->addrsz = addr_size;
    instr->flags = prefix_rep == 2 ? FD_FLAG_REP :
                   prefix_rep == 3 ? FD_FLAG_REPNZ : 0;
    if (mode == DECODE_64)
        instr->flags |= FD_FLAG_64;
    instr->address = address;

    for (unsigned i = 0; i < sizeof(instr->operands) / sizeof(FdOp); i++)
        instr->operands[i] = (FdOp) {0};

    if (DESC_MODRM(desc) && UNLIKELY(off++ >= len))
        return FD_ERR_PARTIAL;
    unsigned op_byte = buffer[off - 1] | (!DESC_MODRM(desc) ? 0xc0 : 0);

    if (UNLIKELY(prefix_evex)) {
        // VSIB inst (gather/scatter) without mask register or w/EVEX.z is UD
        if (DESC_VSIB(desc) && (!(prefix_evex & 0x07) || (prefix_evex & 0x80)))
            return FD_ERR_UD;
        // Inst doesn't support masking, so EVEX.z or EVEX.aaa is UD
        if (!DESC_EVEX_MASK(desc) && (prefix_evex & 0x87))
            return FD_ERR_UD;
        // EVEX.z without EVEX.aaa is UD. The Intel SDM is rather unprecise
        // about this, but real hardware doesn't accept this.
        if ((prefix_evex & 0x87) == 0x80)
            return FD_ERR_UD;

        // Cases for SAE/RC (reg operands only):
        //  - ER supported -> all ok
        //  - SAE supported -> assume L'L is RC, but ignored (undocumented)
        //  - Neither supported -> b == 0
        if ((prefix_evex & 0x10) && (op_byte & 0xc0) == 0xc0) { // EVEX.b+reg
            if (!DESC_EVEX_SAE(desc))
                return FD_ERR_UD;
            vexl = 2;
            if (DESC_EVEX_ER(desc))
                instr->evex = prefix_evex;
            else
                instr->evex = (prefix_evex & 0x87) | 0x60; // set RC, clear B
        } else {
            if (UNLIKELY(vexl == 3)) // EVEX.L'L == 11b is UD
                return FD_ERR_UD;
            instr->evex = prefix_evex & 0x87; // clear RC, clear B
        }

        if (DESC_VSIB(desc))
            vex_operand &= 0xf; // EVEX.V' is used as index extension instead.
    } else {
        instr->evex = 0;
    }

    unsigned op_size;
    unsigned op_size_alt = 0;
    if (!(DESC_OPSIZE(desc) & 4)) {
        if (DESC_OPSIZE(desc) == 1)
            op_size = 1;
        else if (mode == DECODE_64)
            op_size = ((prefix_rex & PREFIX_REXW) || DESC_OPSIZE(desc) == 3) ? 4 :
                                    UNLIKELY(prefix_66 && !DESC_IGN66(desc)) ? 2 :
                                                           DESC_OPSIZE(desc) ? 4 :
                                                                               3;
        else
            op_size = UNLIKELY(prefix_66 && !DESC_IGN66(desc)) ? 2 : 3;
    } else {
        op_size = 5 + vexl;
        op_size_alt = op_size - (DESC_OPSIZE(desc) & 3);
    }

    uint8_t operand_sizes[4] = {
        DESC_SIZE_FIX1(desc), DESC_SIZE_FIX2(desc) + 1, op_size, op_size_alt
    };

    if (UNLIKELY(instr->type == FDI_MOV_CR || instr->type == FDI_MOV_DR)) {
        unsigned modreg = (op_byte >> 3) & 0x7;
        unsigned modrm = op_byte & 0x7;

        FdOp* op_modreg = &instr->operands[DESC_MODREG_IDX(desc)];
        op_modreg->type = FD_OT_REG;
        op_modreg->size = op_size;
        op_modreg->reg = modreg | (prefix_rex & PREFIX_REXR ? 8 : 0);
        op_modreg->misc = instr->type == FDI_MOV_CR ? FD_RT_CR : FD_RT_DR;
        if (instr->type == FDI_MOV_CR && (~0x011d >> op_modreg->reg) & 1)
            return FD_ERR_UD;
        else if (instr->type == FDI_MOV_DR && prefix_rex & PREFIX_REXR)
            return FD_ERR_UD;

        FdOp* op_modrm = &instr->operands[DESC_MODRM_IDX(desc)];
        op_modrm->type = FD_OT_REG;
        op_modrm->size = op_size;
        op_modrm->reg = modrm | (prefix_rex & PREFIX_REXB ? 8 : 0);
        op_modrm->misc = FD_RT_GPL;
        goto skip_modrm;
    }

    if (DESC_HAS_MODREG(desc))
    {
        FdOp* op_modreg = &instr->operands[DESC_MODREG_IDX(desc)];
        unsigned reg_idx = (op_byte & 0x38) >> 3;
        unsigned reg_ty = DESC_REGTY_MODREG(desc);
        op_modreg->misc = reg_ty;
        if (LIKELY(reg_ty < 2))
            reg_idx += prefix_rex & PREFIX_REXR ? 8 : 0;
        else if (reg_ty == 7 && (prefix_rex & PREFIX_REXR || prefix_evex & 0x80))
            return FD_ERR_UD; // REXR in 64-bit mode or EVEX.z with mask as dest
        if (UNLIKELY(reg_ty == FD_RT_VEC)) // REXRR ignored above in 32-bit mode
            reg_idx += prefix_rex & PREFIX_REXRR ? 16 : 0;
        else if (UNLIKELY(prefix_rex & PREFIX_REXRR))
            return FD_ERR_UD;
        op_modreg->type = FD_OT_REG;
        op_modreg->size = operand_sizes[(desc->operand_sizes >> 2) & 3];
        op_modreg->reg = reg_idx;
    }

    if (DESC_HAS_MODRM(desc))
    {
        FdOp* op_modrm = &instr->operands[DESC_MODRM_IDX(desc)];
        op_modrm->size = operand_sizes[(desc->operand_sizes >> 0) & 3];

        unsigned rm = op_byte & 0x07;
        if (op_byte >= 0xc0)
        {
            uint8_t reg_idx = rm;
            unsigned reg_ty = DESC_REGTY_MODRM(desc);
            op_modrm->misc = reg_ty;
            if (LIKELY(reg_ty < 2))
                reg_idx += prefix_rex & PREFIX_REXB ? 8 : 0;
            if (prefix_evex && reg_ty == 0) // vector registers only
                reg_idx += prefix_rex & PREFIX_REXX ? 16 : 0;
            op_modrm->type = FD_OT_REG;
            op_modrm->reg = reg_idx;
        }
        else
        {
            unsigned mod = op_byte & 0xc0;
            bool vsib = UNLIKELY(DESC_VSIB(desc));

            // SIB byte
            uint8_t base = rm;
            if (rm == 4)
            {
                if (UNLIKELY(off >= len))
                    return FD_ERR_PARTIAL;
                uint8_t sib = buffer[off++];
                unsigned scale = sib & 0xc0;
                unsigned idx = (sib & 0x38) >> 3;
                idx += prefix_rex & PREFIX_REXX ? 8 : 0;
                base = sib & 0x07;
                if (!vsib && idx == 4)
                    idx = FD_REG_NONE;
                if (vsib && prefix_evex) {
                    // EVEX.V':EVEX.X:SIB.idx
                    idx |= prefix_evex & 0x8 ? 0 : 0x10;
                }
                op_modrm->misc = scale | idx;
            }
            else
            {
                // VSIB must have a memory operand with SIB byte.
                if (vsib)
                    return FD_ERR_UD;
                op_modrm->misc = FD_REG_NONE;
            }

            // EVEX.z for memory destination operand is UD.
            if (UNLIKELY(prefix_evex & 0x80) && DESC_MODRM_IDX(desc) == 0)
                return FD_ERR_UD;

            // RIP-relative addressing only if SIB-byte is absent
            if (mod == 0 && rm == 5 && mode == DECODE_64)
                op_modrm->reg = FD_REG_IP;
            else if (mod == 0 && base == 5)
                op_modrm->reg = FD_REG_NONE;
            else
                op_modrm->reg = base + (prefix_rex & PREFIX_REXB ? 8 : 0);

            // EVEX.b for memory-operand without broadcast support is UD.
            unsigned scale = 0;
            if (UNLIKELY(prefix_evex & 0x10)) {
                if (UNLIKELY(!DESC_EVEX_BCST(desc)))
                    return FD_ERR_UD;
                if (UNLIKELY(DESC_EVEX_BCST16(desc)))
                    scale = 1;
                else
                    scale = prefix_rex & PREFIX_REXW ? 3 : 2;
                instr->segment |= scale << 6; // Store broadcast size
                op_modrm->type = FD_OT_MEMBCST;
            } else {
                if (UNLIKELY(prefix_evex))
                    scale = op_modrm->size - 1;
                op_modrm->type = FD_OT_MEM;
            }

            if (op_byte & 0x40)
            {
                if (UNLIKELY(off + 1 > len))
                    return FD_ERR_PARTIAL;
                instr->disp = (int8_t) LOAD_LE_1(&buffer[off]) << scale;
                off += 1;
            }
            else if (op_byte & 0x80 || (mod == 0 && base == 5))
            {
                if (UNLIKELY(off + 4 > len))
                    return FD_ERR_PARTIAL;
                instr->disp = (int32_t) LOAD_LE_4(&buffer[off]);
                off += 4;
            }
            else
            {
                instr->disp = 0;
            }
        }
    }

    if (UNLIKELY(DESC_HAS_VEXREG(desc)))
    {
        // Without VEX prefix, this encodes an implicit register
        FdOp* operand = &instr->operands[DESC_VEXREG_IDX(desc)];
        operand->type = FD_OT_REG;
        operand->size = operand_sizes[(desc->operand_sizes >> 4) & 3];
        if (mode == DECODE_32)
            vex_operand &= 0x7;
        // Note: 32-bit will never UD here. EVEX.V' is caught above already.
        // Note: UD if > 16 for non-VEC. No EVEX-encoded instruction uses
        // EVEX.vvvv to refer to non-vector registers. Verified in parseinstrs.
        operand->reg = vex_operand | DESC_ZEROREG_VAL(desc);

        unsigned reg_ty = DESC_REGTY_VEXREG(desc); // VEC GPL MSK FPU
        // In 64-bit mode: UD if FD_RT_MASK and vex_operand&8 != 0
        if (reg_ty == 2 && vex_operand >= 8)
            return FD_ERR_UD;
        operand->misc = (04710 >> (3 * reg_ty)) & 0x7;
    }
    else if (vex_operand != 0)
    {
        // TODO: bit 3 ignored in 32-bit mode? unverified
        return FD_ERR_UD;
    }

    uint32_t imm_control = UNLIKELY(DESC_IMM_CONTROL(desc));
    if (UNLIKELY(imm_control == 1))
    {
        // 1 = immediate constant 1, used for shifts
        FdOp* operand = &instr->operands[DESC_IMM_IDX(desc)];
        operand->type = FD_OT_IMM;
        operand->size = 1;
        instr->imm = 1;
    }
    else if (UNLIKELY(imm_control == 2))
    {
        // 2 = memory, address-sized, used for mov with moffs operand
        FdOp* operand = &instr->operands[DESC_IMM_IDX(desc)];
        operand->type = FD_OT_MEM;
        operand->size = op_size;
        operand->reg = FD_REG_NONE;
        operand->misc = FD_REG_NONE;

        int moffsz = 1 << addr_size;
        if (UNLIKELY(off + moffsz > len))
            return FD_ERR_PARTIAL;
        if (moffsz == 2)
            instr->disp = LOAD_LE_2(&buffer[off]);
        if (moffsz == 4)
            instr->disp = LOAD_LE_4(&buffer[off]);
        if (LIKELY(moffsz == 8))
            instr->disp = LOAD_LE_8(&buffer[off]);
        off += moffsz;
    }
    else if (UNLIKELY(imm_control == 3))
    {
        // 3 = register in imm8[7:4], used for RVMR encoding with VBLENDVP[SD]
        FdOp* operand = &instr->operands[DESC_IMM_IDX(desc)];
        operand->type = FD_OT_REG;
        operand->size = op_size;
        operand->misc = FD_RT_VEC;

        if (UNLIKELY(off + 1 > len))
            return FD_ERR_PARTIAL;
        uint8_t reg = (uint8_t) LOAD_LE_1(&buffer[off]);
        off += 1;

        if (mode == DECODE_32)
            reg &= 0x7f;
        operand->reg = reg >> 4;
        instr->imm = reg & 0x0f;
    }
    else if (imm_control != 0)
    {
        // 4/5 = immediate, operand-sized/8 bit
        // 6/7 = offset, operand-sized/8 bit (used for jumps/calls)
        int imm_byte = imm_control & 1;
        int imm_offset = imm_control & 2;

        FdOp* operand = &instr->operands[DESC_IMM_IDX(desc)];
        operand->type = FD_OT_IMM;

        if (imm_byte) {
            if (UNLIKELY(off + 1 > len))
                return FD_ERR_PARTIAL;
            instr->imm = (int8_t) LOAD_LE_1(&buffer[off++]);
            operand->size = desc->operand_sizes & 0x40 ? 1 : op_size;
        } else {
            operand->size = operand_sizes[(desc->operand_sizes >> 6) & 3];

            uint8_t imm_size;
            if (UNLIKELY(instr->type == FDI_RET || instr->type == FDI_RETF ||
                         instr->type == FDI_SSE_EXTRQ ||
                         instr->type == FDI_SSE_INSERTQ))
                imm_size = 2;
            else if (UNLIKELY(instr->type == FDI_JMPF || instr->type == FDI_CALLF))
                imm_size = (1 << op_size >> 1) + 2;
            else if (UNLIKELY(instr->type == FDI_ENTER))
                imm_size = 3;
            else if (instr->type == FDI_MOVABS)
                imm_size = (1 << op_size >> 1);
            else
                imm_size = op_size == 2 ? 2 : 4;

            if (UNLIKELY(off + imm_size > len))
                return FD_ERR_PARTIAL;

            if (imm_size == 2)
                instr->imm = (int16_t) LOAD_LE_2(&buffer[off]);
            else if (imm_size == 3)
                instr->imm = LOAD_LE_3(&buffer[off]);
            else if (imm_size == 4)
                instr->imm = (int32_t) LOAD_LE_4(&buffer[off]);
            else if (imm_size == 6)
                instr->imm = LOAD_LE_4(&buffer[off]) | LOAD_LE_2(&buffer[off+4]) << 32;
            else if (imm_size == 8)
                instr->imm = (int64_t) LOAD_LE_8(&buffer[off]);
            off += imm_size;
        }

        if (imm_offset)
        {
            if (instr->address != 0)
                instr->imm += instr->address + off;
            else
                operand->type = FD_OT_OFF;
        }
    }

    if (instr->type == FDI_XCHG_NOP)
    {
        // Only 4890, 90, and 6690 are true NOPs.
        if (instr->operands[0].reg == 0 && instr->operands[1].reg == 0)
        {
            instr->operands[0].type = FD_OT_NONE;
            instr->operands[1].type = FD_OT_NONE;
            instr->type = FDI_NOP;
        }
        else
        {
            instr->type = FDI_XCHG;
        }
    }

    if (UNLIKELY(instr->type == FDI_3DNOW))
    {
        unsigned opc3dn = instr->imm;
        if (opc3dn & 0x40)
            return FD_ERR_UD;
        uint64_t msk = opc3dn & 0x80 ? 0x88d144d144d14400 : 0x30003000;
        if (!(msk >> (opc3dn & 0x3f) & 1))
            return FD_ERR_UD;
    }

skip_modrm:
    if (UNLIKELY(prefix_lock)) {
        if (!DESC_LOCK(desc) || instr->operands[0].type != FD_OT_MEM)
            return FD_ERR_UD;
        instr->flags |= FD_FLAG_LOCK;
    }

    if (UNLIKELY(op_size == 1 || instr->type == FDI_MOVSX || instr->type == FDI_MOVZX)) {
        if (!(prefix_rex & PREFIX_REX)) {
            for (int i = 0; i < 2; i++) {
                FdOp* operand = &instr->operands[i];
                if (operand->type == FD_OT_NONE)
                    break;
                if (operand->type == FD_OT_REG && operand->misc == FD_RT_GPL &&
                    operand->size == 1 && operand->reg >= 4)
                    operand->misc = FD_RT_GPH;
            }
        }
    }

    instr->size = off;
    instr->operandsz = DESC_INSTR_WIDTH(desc) ? op_size - 1 : 0;

    return off;
}

uint8_t readZ80_EMU(uint16_t addr) {
    uint8_t val = readZ80(addr);
    add_cycles(3);
    return val;
}

void writeZ80_EMU(uint16_t addr, uint8_t val) {
    writeZ80(addr,val);
    add_cycles(3);
}

void readZ80_EMU_dirty(uint16_t addr, size_t amt) {
    for (size_t i = 0; i < amt; i++) {
        readZ80(addr);
        add_cycles(1);
    }
}

void writeZ80_EMU_dirty(uint16_t addr, uint8_t val, size_t amt) {
    for (size_t i = 0; i < amt; i++) {
        writeZ80(addr,val);
        add_cycles(1);
    }
}

#define REG_BC (regs.b<<8|regs.c)
#define REG_DE (regs.d<<8|regs.e)
#define REG_HL (regs.h<<8|regs.l)

#define write_r16(a,b,x) { uint16_t temp = x; a = temp>>8&0xff; b = temp&0xff; }

#define inc_R { regs.r = (regs.r&0x80)|((regs.r+1)&0x7f); }

static inline void ret(bool cond, bool add_cycle) {
    if (add_cycle) add_cycles(1);
    if (cond) {
        regs.pc = (uint16_t)readZ80_EMU(regs.sp++);
        regs.pc |= (uint16_t)readZ80_EMU(regs.sp++)<<8;
    }
}

static inline void jp(bool cond) {
    uint16_t imm = (uint16_t)read_PC();
    imm |= (uint16_t)read_PC()<<8;
    if (cond) regs.pc = imm;
}

static inline void jr(bool cond, bool add_cycle) {
    int8_t off = (int8_t)read_PC();
    if (cond) {
        readZ80_EMU_dirty(regs.pc-1,5);
        regs.pc += off;
    }
}

static inline void call(uint16_t addr) {
    readZ80_EMU_dirty(regs.pc-1,1);
    writeZ80_EMU(--regs.sp,regs.pc>>8);
    writeZ80_EMU(--regs.sp,regs.pc&0xff);
    regs.pc = addr;
}

extern bool paused;

// call function but it reads the immidiate from memory
// and it has conditionals (ooooh)
static inline void call_cond(bool cond, bool add_cycle) {
    uint16_t imm = (uint16_t)read_PC();
    imm |= (uint16_t)read_PC()<<8;   
    if (cond) {
        call(imm);
    }
}

// for the X and Y flags
static inline void setXYF(uint8_t result) {
    flags.x = result>>5&1;
    flags.y = result>>3&1;
}

static inline void setXYF2(uint8_t result) {
    flags.x = result>>3&1;
    flags.y = result>>5&1;
}

// ummm, add two numbers + carry i guess
static inline uint8_t add8(uint8_t a, uint8_t b, bool c) {
    uint16_t result = a+b+(c&1);
    flags.h = ((a&15)+(b&15)+(c&1))>>4&1; // &16
    flags.c = result>>8&1;
    if (((a&128) == (b&128)) && ((a&128) != (result&128))) // parity flag is weird...
        flags.p = 1;
    else
        flags.p = 0;
    flags.n = 0;
    flags.s = result>>7&1;
    flags.z = (result&0xff) == 0;

    // set x and y flags
    setXYF(result);
    return result&0xff;
}

// basically the opposite of add (no shit sherlock)
static inline uint8_t sub8(uint8_t a, uint8_t b, bool c) {
    uint8_t result = add8(a,~b,!c);
    flags.c = !flags.c;
    flags.h = !flags.h;
    flags.n = 1;
    return result;
}

static inline uint16_t add16(uint16_t a, uint16_t b, bool c) {
    uint8_t lo = add8(a&0xff,b&0xff,c&1);
    uint8_t hi = add8(a>>8&0xff,b>>8&0xff,flags.c&1);
    uint16_t result = lo+(hi<<8);
    flags.z = (result&0xffff) == 0;
    return result&0xffff;
}

static inline uint16_t sub16(uint16_t a, uint16_t b, bool c) {
    uint8_t lo = sub8(a&0xff,b&0xff,c&1);
    uint8_t hi = sub8(a>>8&0xff,b>>8&0xff,flags.c&1);
    uint16_t result = lo+(hi<<8);
    flags.z = (result&0xffff) == 0;
    return result&0xffff;
}


// basically sub but without modifiying A
static inline void cp(uint8_t a, uint8_t b) {
    sub8(a,b,0);
    setXYF(b);
}

// for aluop
static inline bool parity(uint8_t val) {
    uint8_t result = 0;
    for (size_t i = 0; i < 8; i++)
        result ^= (val>>i)&1;
    return result == 0;
}

static inline uint8_t and8(uint8_t a, uint8_t b) {
    uint8_t result = a&b;
    flags.c = 0;
    flags.n = 0;
    flags.h = 1;
    flags.s = result>>7&1;
    flags.z = result == 0;
    setXYF(result);
    flags.p = parity(result);
    return result;
}


static inline uint8_t xor8(uint8_t a, uint8_t b) {
    uint8_t result = a^b;
    flags.c = 0;
    flags.n = 0;
    flags.h = 0;
    flags.s = result>>7&1;
    flags.z = result == 0;
    setXYF(result);
    flags.p = parity(result);
    return result;
}

static inline uint8_t or8(uint8_t a, uint8_t b) {
    uint8_t result = a|b;
    flags.c = 0;
    flags.n = 0;
    flags.h = 0;
    flags.s = result>>7&1;
    flags.z = result == 0;
    setXYF(result);
    flags.p = parity(result);
    return result;
}

// for "pop r16" opcodes
static inline uint16_t pop() {
    uint16_t imm = (uint16_t)readZ80_EMU(regs.sp++);
    imm |= (uint16_t)readZ80_EMU(regs.sp++)<<8;
    return imm;
}

// for "push r16" opcodes
static inline void push(uint16_t val) {
    add_cycles(1);
    writeZ80_EMU(--regs.sp,val>>8);
    writeZ80_EMU(--regs.sp,val&0xff);
}

// for "add hl, r16" opcodes
static inline void addhl(uint16_t val) {
    uint8_t flag_s = flags.s;
    uint8_t flag_z = flags.z;
    uint8_t flag_p = flags.p;

    uint16_t result = add16(REG_HL,val,0);

    flags.s = flag_s;
    flags.z = flag_z;
    flags.p = flag_p;

    write_r16(regs.h,regs.l,result);
}

// for "sbc hl, r16" opcodes
static inline void sbchl(uint16_t val) {
    add_cycles(11-4);
    uint16_t result = sub16(REG_HL,val,flags.c&1);
    flags.z = result == 0;
    flags.s = result>>15&1;
    write_r16(regs.h,regs.l,result);
}

// for "adc hl, r16" opcodes
static inline void adchl(uint16_t val) {
    add_cycles(11-4);
    uint16_t result = add16(REG_HL,val,flags.c&1);
    flags.z = result == 0;
    flags.s = result>>15&1;
    write_r16(regs.h,regs.l,result);
}

// same thing for IX and IY prefixed opcodes
static inline uint16_t addIX_IY(uint16_t ind, uint16_t val) {
    uint8_t flag_s = flags.s;
    uint8_t flag_z = flags.z;
    uint8_t flag_p = flags.p;

    uint16_t result = add16(ind,val,0);

    flags.s = flag_s;
    flags.z = flag_z;
    flags.p = flag_p;

    return result;
}


uint16_t read_AF() {
    uint16_t val = regs.a<<8;
    if (flags.c) val |= 1;
    if (flags.n) val |= 2;
    if (flags.p) val |= 4;
    if (flags.y) val |= 8;
    if (flags.h) val |= 16;
    if (flags.x) val |= 32;
    if (flags.z) val |= 64;
    if (flags.s) val |= 128;
    return val;
}

static inline void write_AF(uint16_t val) {
    regs.a = val>>8;
    flags.c = val&1;
    flags.n = val>>1&1;
    flags.p = val>>2&1;
    flags.y = val>>3&1;
    flags.h = val>>4&1;
    flags.x = val>>5&1;
    flags.z = val>>6&1;
    flags.s = val>>7&1;
}

static inline void halt() {
    regs.halt = 1;
}

const uint8_t ED_cycle_lut[256] = {
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,12,12,15,20,8,14,8,9,12,12,15,20,8,14,8,9,12,12,15,20,8,14,8,9,12,12,15,20,8,14,8,9,12,12,15,20,8,14,8,18,12,12,15,20,8,14,8,18,12,12,15,20,8,14,8,8,12,12,15,20,8,14,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,16,16,16,16,8,8,8,8,16,16,16,16,8,8,8,8,16,16,16,16,8,8,8,8,16,16,16,16,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8    
};

static inline void EDprefix(uint8_t opcode) {
    inc_R;
    add_cycles(1);
    switch (opcode) {
        case 0x43: { // ld (nn), bc
            uint16_t addr = read16();
            writeZ80_EMU(addr, regs.c);
            writeZ80_EMU(addr+1, regs.b);
            break;
        }

        case 0x53: { // ld (nn), de
            uint16_t addr = read16();
            writeZ80_EMU(addr, regs.e);
            writeZ80_EMU(addr+1, regs.d);
            break;
        }

        case 0x63: { // ld (nn), hl
            uint16_t addr = read16();
            writeZ80_EMU(addr, regs.l);
            writeZ80_EMU(addr+1, regs.h);
            break;
        }

        case 0x6b: { // ld hl, (nn)
            uint16_t addr = read16();
            regs.l = readZ80_EMU(addr);
            regs.h = readZ80_EMU(addr+1);
            break;
        }

        case 0x7d: // retn**
        case 0x75:
        case 0x6d:
        case 0x65:
        case 0x5d:
        case 0x55:
        case 0x45: ret(true,false); regs.iff1 = regs.iff2; break; // retn
        case 0x4d: ret(true,false); break; // reti (stubbed to be ret)
        
        case 0x4a: adchl(REG_BC); break; // add hl, bc
        case 0x5a: adchl(REG_DE); break; // add hl, de
        case 0x6a: adchl(REG_HL); break; // add hl, hl
        case 0x7a: adchl(regs.sp); break; // add hl, sp

        case 0x42: sbchl(REG_BC); break; // add hl, bc
        case 0x52: sbchl(REG_DE); break; // add hl, de
        case 0x62: sbchl(REG_HL); break; // add hl, hl
        case 0x72: sbchl(regs.sp); break; // add hl, sp

        case 0x73: { // ld (nn), sp
            uint16_t addr = read16();
            writeZ80_EMU(addr, regs.sp&0xff);
            writeZ80_EMU(addr+1, regs.sp>>8&0xff);
            break;
        }

        case 0x4B: { // ld bc, (nn)
            uint16_t addr = read16();
            regs.c = readZ80_EMU(addr);
            regs.b = readZ80_EMU(addr+1);
            break;
        }

        case 0x5B: { // ld de, (nn)
            uint16_t addr = read16();
            regs.e = readZ80_EMU(addr);
            regs.d = readZ80_EMU(addr+1);
            break;
        }

        case 0x7B: { // ld sp, (nn)
            uint16_t addr = read16();
            regs.sp = readZ80_EMU(addr)|(readZ80_EMU(addr+1)<<8);
            break;
        }

        case 0xA0: { // ldi
            uint8_t read_val = readZ80_EMU(REG_HL);
            uint8_t n = regs.a+read_val;
            writeZ80_EMU(REG_DE,read_val);
            writeZ80_EMU_dirty(REG_DE,read_val,2);
            write_r16(regs.d,regs.e,REG_DE+1);
            write_r16(regs.h,regs.l,REG_HL+1);
            write_r16(regs.b,regs.c,REG_BC-1);
            flags.x = n>>1&1;
            flags.y = n>>3&1;
            flags.n = 0;
            flags.p = (regs.b<<8|regs.c) != 0;
            flags.h = 0;
            break;
        }

        case 0xB0: { // ldir
            uint8_t read_val = readZ80_EMU(REG_HL);
            uint8_t n = regs.a+read_val;
            writeZ80_EMU(REG_DE,read_val);
            writeZ80_EMU_dirty(REG_DE,read_val,2);
            write_r16(regs.d,regs.e,REG_DE+1);
            write_r16(regs.h,regs.l,REG_HL+1);
            write_r16(regs.b,regs.c,REG_BC-1);
            flags.x = n>>1&1;
            flags.y = n>>3&1;
            flags.n = 0;
            flags.p = (regs.b<<8|regs.c) != 0;
            flags.h = 0;
            if ((regs.b<<8|regs.c) != 0) {
                regs.pc -= 2;
                writeZ80_EMU_dirty(REG_DE-1,read_val,5);
                //add_cycles(5);
            }
            break;
        }

        case 0xA9: { // cpd
            bool flags_c = flags.c;
            uint8_t val = readZ80_EMU(REG_HL);
            uint8_t result = sub8(regs.a,val,0);
            readZ80_EMU_dirty(REG_HL,5);
            flags.c = flags_c;
            uint8_t flagxy = regs.a-val-(flags.h?1:0);
            flags.x = flagxy>>1&1;
            flags.y = flagxy>>3&1;
            write_r16(regs.h,regs.l,REG_HL-1);
            write_r16(regs.b,regs.c,REG_BC-1);
            flags.p = (regs.b<<8|regs.c) != 0;
            break;
        }

        case 0xB9: { // cpdr
            bool flags_c = flags.c;
            uint8_t val = readZ80_EMU(REG_HL);
            uint8_t result = sub8(regs.a,val,0);
            readZ80_EMU_dirty(REG_HL,5);
            flags.c = flags_c;
            uint8_t flagxy = regs.a-val-(flags.h?1:0);
            flags.x = flagxy>>1&1;
            flags.y = flagxy>>3&1;
            write_r16(regs.h,regs.l,REG_HL-1);
            write_r16(regs.b,regs.c,REG_BC-1);
            flags.p = (regs.b<<8|regs.c) != 0;
            if (((regs.b<<8|regs.c) != 0) && !flags.z) {
                regs.pc -= 2;
                readZ80_EMU_dirty(REG_HL,5);
            }
            break;
        }

        case 0xA2: { // ini
            add_cycles(1);
            uint8_t read_val = inZ80(REG_BC);
            writeZ80_EMU(REG_HL,read_val);
            regs.b = sub8(regs.b,1,0);
            flags.n = 1;
            write_r16(regs.h,regs.l,REG_HL+1);
            break;
        }

        case 0xB2: { // inir
            add_cycles(1);
            uint8_t read_val = inZ80(REG_BC);
            writeZ80_EMU(REG_HL,read_val);
            regs.b = sub8(regs.b,1,0);
            flags.n = 1;
            write_r16(regs.h,regs.l,REG_HL+1);
            if (regs.b != 0) {
                regs.pc -= 2;
                writeZ80_EMU_dirty(REG_HL-1,read_val,5);
            }
            break;
        }

        case 0xAA: { // ind
            add_cycles(1);
            uint8_t read_val = inZ80(REG_BC);
            writeZ80_EMU(REG_HL,read_val);
            regs.b = sub8(regs.b,1,0);
            flags.n = 1;
            write_r16(regs.h,regs.l,REG_HL-1);
            break;
        }

        case 0xBA: { // indr
            add_cycles(1);
            uint8_t read_val = inZ80(REG_BC);
            writeZ80_EMU(REG_HL,read_val);
            regs.b = sub8(regs.b,1,0);
            flags.n = 1;
            write_r16(regs.h,regs.l,REG_HL-1);
            if (regs.b != 0) {
                regs.pc -= 2;
                writeZ80_EMU_dirty(REG_HL+1,read_val,5);
            }
            break;
        }

        case 0xA1: { // cpi
            bool flags_c = flags.c;
            uint8_t val = readZ80_EMU(REG_HL);
            uint8_t result = sub8(regs.a,val,0);
            readZ80_EMU_dirty(REG_HL,5);
            flags.c = flags_c;
            uint8_t flagxy = regs.a-val-(flags.h?1:0);
            flags.x = flagxy>>1&1;
            flags.y = flagxy>>3&1;
            write_r16(regs.h,regs.l,REG_HL+1);
            write_r16(regs.b,regs.c,REG_BC-1);
            flags.p = (regs.b<<8|regs.c) != 0;
            break;
        }

        case 0xB1: { // cpir
            bool flags_c = flags.c;
            uint8_t val = readZ80_EMU(REG_HL);
            uint8_t result = sub8(regs.a,val,0);
            readZ80_EMU_dirty(REG_HL,5);
            flags.c = flags_c;
            uint8_t flagxy = regs.a-val-(flags.h?1:0);
            flags.x = flagxy>>1&1;
            flags.y = flagxy>>3&1;
            write_r16(regs.h,regs.l,REG_HL+1);
            write_r16(regs.b,regs.c,REG_BC-1);
            flags.p = (regs.b<<8|regs.c) != 0;
            if (((regs.b<<8|regs.c) != 0) && !flags.z) {
                regs.pc -= 2;
                readZ80_EMU_dirty(REG_HL,5);
            }
            break;
        }

        case 0xA8: { // ldd
            uint8_t read_val = readZ80_EMU(REG_HL);
            uint8_t n = regs.a+read_val;
            writeZ80_EMU(REG_DE,read_val);
            writeZ80_EMU_dirty(REG_DE,read_val,2);
            write_r16(regs.d,regs.e,REG_DE-1);
            write_r16(regs.h,regs.l,REG_HL-1);
            write_r16(regs.b,regs.c,REG_BC-1);
            flags.x = n>>1&1;
            flags.y = n>>3&1;
            flags.n = 0;
            flags.p = (regs.b<<8|regs.c) != 0;
            flags.h = 0;
            break;
        }

        case 0xB8: { // lddr
            uint8_t read_val = readZ80_EMU(REG_HL);
            uint8_t n = regs.a+read_val;
            writeZ80_EMU(REG_DE,read_val);
            writeZ80_EMU_dirty(REG_DE,read_val,2);
            write_r16(regs.d,regs.e,REG_DE-1);
            write_r16(regs.h,regs.l,REG_HL-1);
            write_r16(regs.b,regs.c,REG_BC-1);
            flags.x = n>>1&1;
            flags.y = n>>3&1;
            flags.n = 0;
            flags.p = (regs.b<<8|regs.c) != 0;
            flags.h = 0;
            if ((regs.b<<8|regs.c) != 0) {
                regs.pc -= 2;
                writeZ80_EMU_dirty(REG_DE+1,read_val,5);        
            }
            break;
        }

        case 0xA3: { // outi
            add_cycles(1);
            uint8_t val = readZ80_EMU(REG_HL);
            regs.b = sub8(regs.b,1,0);
            outZ80(REG_BC,val);
            write_r16(regs.h,regs.l,REG_HL+1);
            flags.n = val>>7;
            flags.c = (val+regs.l)>255;
            //flags.h = flags.c;
            flags.p = parity(((val+regs.l)&7)^regs.b);
            setXYF(regs.pc>>8);
            break;
        }

        case 0xB3: { // otir
            add_cycles(1);
            uint8_t val = readZ80_EMU(REG_HL);
            regs.b = sub8(regs.b,1,0);
            outZ80(REG_BC,val);
            write_r16(regs.h,regs.l,REG_HL+1);
            flags.n = val>>7;
            flags.c = (val+regs.l)>255;
            //flags.h = flags.c;
            flags.p = parity(((val+regs.l)&7)^regs.b);
            //regs.b = sub8(regs.b,1,0);
            setXYF(regs.pc>>8);
            if (regs.b != 0) {
                regs.pc -= 2;
                readZ80_EMU_dirty(REG_HL,5);
            }
            break;
        }

        case 0xAB: { // outd
            add_cycles(1);
            uint8_t val = readZ80_EMU(REG_HL);
            regs.b = sub8(regs.b,1,0);
            outZ80(REG_BC,val);
            write_r16(regs.h,regs.l,REG_HL-1);
            flags.n = val>>7;
            flags.c = (val+regs.l)>255;
            //flags.h = flags.c;
            flags.p = parity(((val+regs.l)&7)^regs.b);
            setXYF(regs.pc>>8);
            break;
        }

        case 0xBB: { // otdr
            add_cycles(1);
            uint8_t val = readZ80_EMU(REG_HL);
            regs.b = sub8(regs.b,1,0);
            outZ80(REG_BC,val);
            //regs.b = sub8(regs.b,1,0);
            write_r16(regs.h,regs.l,REG_HL-1);
            flags.n = val>>7;
            flags.c = (val+regs.l)>255;
            flags.h = flags.c;
            flags.p = parity(((val+regs.l)&7)^regs.b);
            setXYF(regs.pc>>8);
            if (regs.b != 0) {
                regs.pc -= 2;
                readZ80_EMU_dirty(REG_HL,5);
            }
            break;
        }

        case 0x7c: // neg* (* == undocumented)
        case 0x74:
        case 0x6c:
        case 0x64:
        case 0x5c:
        case 0x54:
        case 0x4c:
        case 0x44: regs.a = sub8(0,regs.a,0); break; // neg

        case 0x67: { // rrd
            flags.n = 0;
            flags.h = 0;
            uint8_t val = readZ80_EMU(REG_HL);
            readZ80_EMU_dirty(REG_HL,4);
            writeZ80_EMU(REG_HL,(regs.a<<4)|(val>>4));
            regs.a = (regs.a&0xf0)|(val&0xf);
            setXYF(regs.a);
            flags.z = regs.a == 0;
            flags.s = regs.a>>7&1;
            flags.p = parity(regs.a);
            break;
        }

        case 0x6f: { // rld
            flags.n = 0;
            flags.h = 0;
            uint8_t val = readZ80_EMU(REG_HL);
            readZ80_EMU_dirty(REG_HL,4);
            writeZ80_EMU(REG_HL,(val<<4)|(regs.a&15));
            regs.a = (regs.a&0xf0)|(val>>4);
            setXYF(regs.a);
            flags.z = regs.a == 0;
            flags.s = regs.a>>7&1;
            flags.p = parity(regs.a);
            break;
        }

        // out (c), r8
        case 0x41: outZ80(REG_BC,regs.b); break;
        case 0x49: outZ80(REG_BC,regs.c); break;
        case 0x51: outZ80(REG_BC,regs.d); break;
        case 0x59: outZ80(REG_BC,regs.e); break;
        case 0x61: outZ80(REG_BC,regs.h); break;
        case 0x69: outZ80(REG_BC,regs.l); break;
        case 0x71: outZ80(REG_BC,0); break; // out (c), 0
        case 0x79: outZ80(REG_BC,regs.a); break;

        // in r8, (c)
        case 0x40: {
            regs.b = inZ80(REG_BC);
            flags.h = 0;
            flags.n = 0; 
            flags.p = parity(regs.b); 
            flags.z = regs.b == 0;
            flags.s = regs.b>>7&1;
            setXYF(regs.b);
            break;
        }
        case 0x48: {
            regs.c = inZ80(REG_BC);
            flags.h = 0;
            flags.n = 0; 
            flags.p = parity(regs.c); 
            flags.z = regs.c == 0;
            flags.s = regs.c>>7&1;
            setXYF(regs.c);
            break;
        }
        case 0x50: {
            regs.d = inZ80(REG_BC);
            flags.h = 0;
            flags.n = 0; 
            flags.p = parity(regs.d); 
            flags.z = regs.d == 0;
            flags.s = regs.d>>7&1;
            setXYF(regs.d);
            break;
        }
        case 0x58: {
            regs.e = inZ80(REG_BC);
            flags.h = 0;
            flags.n = 0; 
            flags.p = parity(regs.e); 
            flags.z = regs.e == 0;
            flags.s = regs.e>>7&1;
            setXYF(regs.e);
            break;
        }
        case 0x60: {
            regs.h = inZ80(REG_BC);
            flags.h = 0;
            flags.n = 0; 
            flags.p = parity(regs.h); 
            flags.z = regs.h == 0;
            flags.s = regs.h>>7&1;
            setXYF(regs.h);
            break;
        }
        case 0x68: {
            regs.l = inZ80(REG_BC);
            flags.h = 0;
            flags.n = 0; 
            flags.p = parity(regs.l); 
            flags.z = regs.l == 0;
            flags.s = regs.l>>7&1;
            setXYF(regs.l);
            break;
        }

        case 0x70: { // in (c)
            uint8_t reg = inZ80(REG_BC);
            flags.h = 0;
            flags.n = 0; 
            flags.p = parity(reg); 
            flags.z = reg == 0;
            flags.s = reg>>7&1;
            setXYF(reg);
            break;
        }

        case 0x78: { // in a, (c)
            regs.a = inZ80(REG_BC);
            flags.h = 0;
            flags.n = 0; 
            flags.p = parity(regs.a); 
            flags.z = regs.a == 0;
            flags.s = regs.a>>7&1;
            setXYF(regs.a);
            break;
        }

        case 0x6e:
        case 0x66:
        case 0x4e:
        case 0x46: regs.im = 0; break; // IM 0
        
        case 0x76:
        case 0x56: regs.im = 1; break; // IM 1
        
        case 0x7e:
        case 0x5e: regs.im = 2; break; // IM 2

        case 0x47: add_cycles(1); regs.i = regs.a; break; // ld i, a
        case 0x57: // ld a, i
            add_cycles(1);
            regs.a = regs.i;
            flags.p = regs.iff2;
            flags.h = 0;
            flags.n = 0; 
            flags.z = regs.a == 0;
            flags.s = regs.a>>7&1;
            setXYF(regs.a);
            break;
        case 0x4f: add_cycles(1); regs.r = regs.a; break; // ld r, a
        case 0x5f: // ld a, r
            add_cycles(1);
            regs.a = regs.r;
            flags.p = regs.iff2;
            flags.h = 0;
            flags.n = 0; 
            flags.z = regs.a == 0;
            flags.s = regs.a>>7&1;
            setXYF(regs.a);
            break;

        case 0x77: // nop*
        case 0x7f:
            break;

        default:
            printf("\nUNSUPPORTED ED OPCODE %04x: %02x\n",regs.pc-1,opcode);
            //ula.quit = 1; // exit(0);
            break;
    }
}

// for (ix+d) or (iy+d)
static inline uint16_t index_d_read(uint16_t ind) {
    int8_t off = (int8_t)read_PC();
    readZ80_EMU_dirty(regs.pc-1,5);
    return readZ80_EMU(ind+off);
}

static inline uint8_t cb_rot(uint8_t opcode, uint8_t reg) {
    switch (opcode & 0x38) {
        case 0x00: { // rlc
            flags.h = 0;
            flags.n = 0;
            flags.c = reg>>7;
            uint8_t val = (reg<<1)|(reg>>7);
            flags.z = val == 0;
            flags.s = val>>7;
            flags.p = parity(val);
            setXYF(val);
            return val;
        }

        case 0x08: { // rrc
            flags.h = 0;
            flags.n = 0;
            flags.c = reg & 1;
            uint8_t val = ((reg&1)<<7)|(reg>>1);
            flags.z = val == 0;
            flags.s = val>>7;
            flags.p = parity(val);
            setXYF(val);
            return val;
        }

        case 0x10: { // rl
            uint8_t carry = flags.c;
            flags.h = 0;
            flags.n = 0;
            flags.c = reg>>7;
            uint8_t val = carry|(reg<<1);
            flags.z = val == 0;
            flags.s = val>>7;
            flags.p = parity(val);
            setXYF(val);
            return val;
        }

        case 0x18: { // rr
            uint8_t carry = flags.c;
            flags.h = 0;
            flags.n = 0;
            flags.c = reg&1;
            uint8_t val = (carry<<7)|(reg>>1);
            flags.z = val == 0;
            flags.s = val>>7;
            flags.p = parity(val);
            setXYF(val);
            return val;
        }

        case 0x20: { // sla
            uint8_t carry = flags.c;
            flags.h = 0;
            flags.n = 0;
            flags.c = reg>>7;
            uint8_t val = reg<<1;
            flags.z = val == 0;
            flags.s = val>>7;
            flags.p = parity(val);
            setXYF(val);
            return val;
        }

        case 0x28: { // sra
            flags.h = 0;
            flags.n = 0;
            flags.c = reg&1;
            uint8_t val = (reg&0x80)|(reg>>1);
            flags.z = val == 0;
            flags.s = val>>7;
            flags.p = parity(val);
            setXYF(val);
            return val;
        }

        case 0x30: { // sll
            flags.h = 0;
            flags.n = 0;
            flags.c = reg>>7;
            uint8_t val = (reg<<1)|1;
            flags.z = val == 0;
            flags.s = val>>7;
            flags.p = parity(val);
            setXYF(val);
            return val;
        }

        case 0x38: { // srl
            flags.h = 0;
            flags.n = 0;
            flags.c = reg&1;
            uint8_t val = reg>>1;
            flags.z = val == 0;
            flags.s = val>>7;
            flags.p = parity(val);
            setXYF(val);
            return val;
        }
    }
    return 0;
}

static inline void index_cb(uint16_t *ind) {
    inc_R;
    int8_t off = (int8_t)read_PC();
    uint16_t addr = (*ind)+off;
    uint8_t opcode = read_PC();
    readZ80_EMU_dirty(regs.pc-1,2);
    switch (opcode&0xC0) {
        case 0x00: { // rot
            uint8_t val = readZ80_EMU(addr);
            readZ80_EMU_dirty(addr,1);
            val = cb_rot(opcode,val);
            switch (opcode&7) { // write to register
                case 0: regs.b = val; break;
                case 1: regs.c = val; break;
                case 2: regs.d = val; break;
                case 3: regs.e = val; break;
                case 4: regs.h = val; break;
                case 5: regs.l = val; break;
                case 6: break;
                case 7: regs.a = val; break;
            }
            writeZ80_EMU(addr,val);
            //add_cycles(23);
            break;
        }

        case 0x40: { // bit x, (ix+d)
            uint8_t val = readZ80_EMU(addr);
            readZ80_EMU_dirty(addr,1);
            flags.n = 0;
            flags.h = 1;
            val &= 1 << ((opcode>>3)&7);
            setXYF(addr>>8);
            flags.z = val == 0;
            flags.p = val == 0;
            flags.s = val>>7&1;
            //add_cycles(20);
            break;
        }

        case 0x80: { // res x, (ix+d)
            uint8_t val = readZ80_EMU(addr);
            readZ80_EMU_dirty(addr,1);
            val &= ~(1 << ((opcode>>3)&7));
            switch (opcode&7) { // write to register
                case 0: regs.b = val; break;
                case 1: regs.c = val; break;
                case 2: regs.d = val; break;
                case 3: regs.e = val; break;
                case 4: regs.h = val; break;
                case 5: regs.l = val; break;
                case 6: break;
                case 7: regs.a = val; break;
            }
            writeZ80_EMU(addr,val);
            //add_cycles(23);
            break;
        }

        case 0xC0: { // set x, (ix+d)
            uint8_t val = readZ80_EMU(addr);
            readZ80_EMU_dirty(addr,1);
            val |= 1 << ((opcode>>3)&7);
            switch (opcode&7) { // write to register
                case 0: regs.b = val; break;
                case 1: regs.c = val; break;
                case 2: regs.d = val; break;
                case 3: regs.e = val; break;
                case 4: regs.h = val; break;
                case 5: regs.l = val; break;
                case 6: break;
                case 7: regs.a = val; break;
            }
            writeZ80_EMU(addr,val);
            //add_cycles(23);
            break;
        }

        default:
            printf("\nUNSUPPORTED OPCODE IX-IY CB %04x: %02x\n",regs.pc-1,opcode);
            ula.quit = 1; // exit(0);
            break;        
    }
}

static inline void cb_step() {
    inc_R;
    uint8_t opcode = read_PC();
    add_cycles(1);
    uint8_t val;
    switch (opcode&0x07) {
        case 0: val = regs.b; break;
        case 1: val = regs.c; break;
        case 2: val = regs.d; break;
        case 3: val = regs.e; break;
        case 4: val = regs.h; break;
        case 5: val = regs.l; break;
        case 6: val = readZ80_EMU(REG_HL); readZ80_EMU_dirty(REG_HL,1); break;
        case 7: val = regs.a; break;
    }

    //add_cycles(8);

    switch (opcode&0xC0) {
        case 0x00: { // rot
            val = cb_rot(opcode, val);
            break;
        }

        case 0x40: { // bit
            flags.n = 0;
            flags.h = 1;
            setXYF(val);
            val &= 1 << ((opcode>>3)&7);
            flags.z = val == 0;
            flags.p = val == 0;
            flags.s = val>>7&1;
            break;
        }

        case 0x80: { // res
            val &= ~(1 << ((opcode>>3)&7));
            break;
        }

        case 0xC0: { // set
            val |= 1 << ((opcode>>3)&7);
            break;
        }

        default:
            printf("\nUNSUPPORTED OPCODE CB %04x: %02x\n",regs.pc-1,opcode);
            ula.quit = 1; // exit(0);
            break;        
    }

    if ((opcode&0xc0) == 0x40) return;

    switch (opcode&0x07) {
        case 0: regs.b = val; break;
        case 1: regs.c = val; break;
        case 2: regs.d = val; break;
        case 3: regs.e = val; break;
        case 4: regs.h = val; break;
        case 5: regs.l = val; break;
        case 6: writeZ80_EMU(REG_HL,val); break;
        case 7: regs.a = val; break;
    }
}

static inline int index_step(uint16_t *ind, uint8_t opcode) {
    inc_R;
    switch (opcode) {

        // ld reg, #imm
        case 0x26: *ind = ((*ind)&0xff)|(read_PC()<<8); break;
        case 0x2e: *ind = ((*ind)&0xff00)|read_PC(); break;

        // ld src, dst
        case 0x44: regs.b = (*ind)>>8&0xff; break;
        case 0x45: regs.b = (*ind)&0xff; break;
        case 0x46: regs.b = index_d_read(*ind); break;
        case 0x4c: regs.c = (*ind)>>8&0xff; break;
        case 0x4d: regs.c = (*ind)&0xff; break;
        case 0x4e: regs.c = index_d_read(*ind); break;
        case 0x54: regs.d = (*ind)>>8&0xff; break;
        case 0x55: regs.d = (*ind)&0xff; break;
        case 0x56: regs.d = index_d_read(*ind); break;
        case 0x5c: regs.e = (*ind)>>8&0xff; break;
        case 0x5d: regs.e = (*ind)&0xff; break;
        case 0x5e: regs.e = index_d_read(*ind); break;
        case 0x60: *ind = (regs.b<<8)|((*ind)&0xff); break;
        case 0x61: *ind = (regs.c<<8)|((*ind)&0xff); break;
        case 0x62: *ind = (regs.d<<8)|((*ind)&0xff); break;
        case 0x63: *ind = (regs.e<<8)|((*ind)&0xff); break;
        case 0x64: break;
        case 0x65: *ind = ((*ind)<<8)|((*ind)&0xff); break;
        case 0x66: regs.h = index_d_read(*ind); break;
        case 0x67: *ind = (regs.a<<8)|((*ind)&0xff); break;
        case 0x68: *ind = (regs.b)|((*ind)&0xff00); break;
        case 0x69: *ind = (regs.c)|((*ind)&0xff00); break;
        case 0x6a: *ind = (regs.d)|((*ind)&0xff00); break;
        case 0x6b: *ind = (regs.e)|((*ind)&0xff00); break;
        case 0x6c: *ind = ((*ind)>>8)|((*ind)&0xff00); break;
        case 0x6d: break;
        case 0x6e: regs.l = index_d_read(*ind); break;
        case 0x6f: *ind = (regs.a)|((*ind)&0xff00); break;

        case 0x70: { // ld (ix+d), b
            int8_t off = (int8_t)read_PC();
            readZ80_EMU_dirty(regs.pc-1,5);
            uint16_t addr = (*ind)+off;
            writeZ80_EMU(addr,regs.b); 
            break;
        }
        case 0x71: { // ld (ix+d), c
            int8_t off = (int8_t)read_PC();
            readZ80_EMU_dirty(regs.pc-1,5);
            uint16_t addr = (*ind)+off;
            writeZ80_EMU(addr,regs.c); 
            break;
        }
        case 0x72: { // ld (ix+d), d
            int8_t off = (int8_t)read_PC();
            readZ80_EMU_dirty(regs.pc-1,5);
            uint16_t addr = (*ind)+off;
            writeZ80_EMU(addr,regs.d); 
            break;
        }
        case 0x73: { // ld (ix+d), e
            int8_t off = (int8_t)read_PC();
            readZ80_EMU_dirty(regs.pc-1,5);
            uint16_t addr = (*ind)+off;
            writeZ80_EMU(addr,regs.e); 
            break;
        }
        case 0x74: { // ld (ix+d), h
            int8_t off = (int8_t)read_PC();
            readZ80_EMU_dirty(regs.pc-1,5);
            uint16_t addr = (*ind)+off;
            writeZ80_EMU(addr,regs.h); 
            break;
        }
        case 0x75: { // ld (ix+d), l
            int8_t off = (int8_t)read_PC();
            readZ80_EMU_dirty(regs.pc-1,5);
            uint16_t addr = (*ind)+off;
            writeZ80_EMU(addr,regs.l); 
            break;
        }
        case 0x77: { // ld (ix+d), a
            int8_t off = (int8_t)read_PC();
            readZ80_EMU_dirty(regs.pc-1,5);
            uint16_t addr = (*ind)+off;
            writeZ80_EMU(addr,regs.a); 
            break;
        }

        case 0x7c: regs.a = (*ind)>>8&0xff; break;
        case 0x7d: regs.a = (*ind)&0xff; break;
        case 0x7e: regs.a = index_d_read(*ind); break;

        case 0x09: add_cycles(11-4); *ind = addIX_IY(*ind,REG_BC); break; // add ix, bc
        case 0x19: add_cycles(11-4); *ind = addIX_IY(*ind,REG_DE); break; // add ix, de
        case 0x29: add_cycles(11-4); *ind = addIX_IY(*ind,*ind); break; // add ix, ix
        case 0x39: add_cycles(11-4); *ind = addIX_IY(*ind,regs.sp); break; // add ix, sp

        case 0x21: *ind = read16(); break; // ld ix, nn

        case 0x2a: { // ld ix, (nn)
            uint16_t addr = read16();
            *ind = readZ80_EMU(addr)|(readZ80_EMU(addr+1)<<8);
            break;
        }

        case 0x22: { // ld (nn), ix
            uint16_t addr = read16();
            writeZ80_EMU(addr, (*ind)&0xff);
            writeZ80_EMU(addr+1, (*ind)>>8&0xff);
            break;
        }

        case 0x23: add_cycles(2); (*ind)++; break; // inc ix
        case 0x2b: add_cycles(2); (*ind)--; break; // dec ix

        case 0x24: { // inc ixh
            bool flags_c = flags.c;
            *ind = ((add8((*ind)>>8&0xff,1,0))<<8)|((*ind)&0xff);
            flags.c = flags_c;
            break;
        }

        case 0x25: { // dec ixh
            bool flags_c = flags.c;
            *ind = ((sub8((*ind)>>8&0xff,1,0))<<8)|((*ind)&0xff);
            flags.c = flags_c;
            break;
        }

        case 0x2c: { // inc ixl
            bool flags_c = flags.c;
            *ind = ((add8((*ind)&0xff,1,0))&0xff)|((*ind)&0xff00);
            flags.c = flags_c;
            break;
        }

        case 0x2d: { // dec ixl
            bool flags_c = flags.c;
            *ind = ((sub8((*ind)&0xff,1,0))&0xff)|((*ind)&0xff00);
            flags.c = flags_c;
            break;
        }

        case 0x34: { // inc (ix+d)
            int8_t off = (int8_t)read_PC();
            readZ80_EMU_dirty(regs.pc-1,5);
            bool flags_c = flags.c; 
            uint16_t addr = (*ind)+off;
            uint8_t val = readZ80_EMU(addr);
            readZ80_EMU_dirty(addr,1);
            writeZ80_EMU(addr,add8(val,1,0)); 
            flags.c = flags_c;
            break;
        }

        case 0x35: { // dec (ix+d)
            int8_t off = (int8_t)read_PC();
            readZ80_EMU_dirty(regs.pc-1,5);
            bool flags_c = flags.c; 
            uint16_t addr = (*ind)+off;
            uint8_t val = readZ80_EMU(addr);
            readZ80_EMU_dirty(addr,1);
            writeZ80_EMU(addr,sub8(val,1,0)); 
            flags.c = flags_c;
            break;
        }

        case 0x36: { // ld (ix+d), n
            int8_t off = (int8_t)read_PC();
            uint8_t data = read_PC();
            readZ80_EMU_dirty(regs.pc-1,2);
            uint16_t addr = (*ind)+off;
            writeZ80_EMU(addr,data); 
            break;
        }

        case 0xe1: *ind = pop(); break; // pop ix
        case 0xe5: push(*ind); break; // push ix

        // aluop ixh/ixl
        case 0x84: regs.a = add8(regs.a,((*ind)>>8)&0xff,0); break;
        case 0x85: regs.a = add8(regs.a,(*ind)&0xff,0); break;
        case 0x8c: regs.a = add8(regs.a,((*ind)>>8)&0xff,flags.c); break;
        case 0x8d: regs.a = add8(regs.a,(*ind)&0xff,flags.c); break;
        case 0x94: regs.a = sub8(regs.a,((*ind)>>8)&0xff,0); break;
        case 0x95: regs.a = sub8(regs.a,(*ind)&0xff,0); break;
        case 0x9c: regs.a = sub8(regs.a,((*ind)>>8)&0xff,flags.c); break;
        case 0x9d: regs.a = sub8(regs.a,(*ind)&0xff,flags.c); break;
        case 0xa4: regs.a = and8(regs.a,((*ind)>>8)&0xff); break;
        case 0xa5: regs.a = and8(regs.a,(*ind)&0xff); break;
        case 0xac: regs.a = xor8(regs.a,((*ind)>>8)&0xff); break;
        case 0xad: regs.a = xor8(regs.a,(*ind)&0xff); break;
        case 0xb4: regs.a = or8(regs.a,((*ind)>>8)&0xff); break;
        case 0xb5: regs.a = or8(regs.a,(*ind)&0xff); break;
        case 0xbc: cp(regs.a,((*ind)>>8)&0xff); break;
        case 0xbd: cp(regs.a,(*ind)&0xff); break;

        // aluop (ix+d)
        case 0x86: regs.a = add8(regs.a,index_d_read(*ind),0); break;
        case 0x8e: regs.a = add8(regs.a,index_d_read(*ind),flags.c); break;
        case 0x96: regs.a = sub8(regs.a,index_d_read(*ind),0); break;
        case 0x9e: regs.a = sub8(regs.a,index_d_read(*ind),flags.c); break;
        case 0xa6: regs.a = and8(regs.a,index_d_read(*ind)); break;
        case 0xae: regs.a = xor8(regs.a,index_d_read(*ind)); break;
        case 0xb6: regs.a = or8(regs.a,index_d_read(*ind)); break;
        case 0xbe: cp(regs.a,index_d_read(*ind)); break;

        case 0xe9: regs.pc = *ind; break; // jp ix
        case 0xf9: add_cycles(2); regs.sp = *ind; break; // ld sp, ix

        case 0xe3: { // ex (sp), ix
            // pc:4,sp:3,sp+1:4,sp(write):3,sp+1(write):3,sp+1(write):1 x 2
            uint8_t temp = readZ80_EMU(regs.sp); 
            uint8_t temp_hi = readZ80_EMU(regs.sp+1); 
            add_cycles(1);
            writeZ80_EMU(regs.sp,(*ind)&0xff);
            (*ind) = ((*ind)&0xff00)|temp;
            writeZ80_EMU(regs.sp+1,((*ind)>>8)&0xff);
            writeZ80_EMU_dirty(regs.sp+1,((*ind)>>8)&0xff,2);
            (*ind) = (((uint16_t)temp_hi)<<8)|((*ind)&0xff);
            break;
        }

        default: {
            //if (opcode >= 0x40 && opcode < 0xC0) return 2;
            //if (opcode < 0xC0) return 2;
            //else return 1;
            return 2;
        }
    }


    return 0;
}

const uint8_t cycle_lut[256] = {
    4,10,7,6,4,4,7,4,4,11,7,6,4,4,7,4,8,10,7,6,4,4,7,4,12,11,7,6,4,4,7,4,7,10,16,6,4,4,7,4,7,11,16,6,4,4,7,4,7,10,13,6,11,11,10,4,7,11,13,6,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,7,7,7,7,7,7,4,7,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,4,4,4,4,4,4,7,4,5,10,10,10,10,11,7,11,5,10,10,0,10,17,7,11,5,10,10,11,10,11,7,11,5,4,10,11,10,0,7,11,5,10,10,19,10,11,7,11,5,4,10,4,10,0,7,11,5,10,10,4,10,11,7,11,5,6,10,4,10,0,7,11
};

const uint8_t IX_IY_cycle_lut[256] = {
    4,4,4,4,4,4,4,4,4,15,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,15,4,4,4,4,4,4,4,14,20,10,8,8,11,4,4,15,20,10,8,8,11,4,4,4,4,4,23,23,19,4,4,15,4,4,4,4,4,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,8,8,8,8,8,8,19,8,8,8,8,8,8,8,19,8,19,19,19,19,19,19,4,19,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,8,8,19,4,4,4,4,4,4,4,4,4,4,4,4,0,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,14,4,23,4,15,4,4,4,8,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,10,4,4,4,4,4,4
};

inline static void check_IRQ(uint8_t opcode);

uint8_t step() {
    if (regs.halt) { // if halted, advance 4 cycles and check IRQ
        add_cycles(4);
        return 0;
    }
    
    uint8_t opcode = read_PC();
do_opcode:
    add_cycles(1);
do_opcode_no_cyc_R:
    inc_R;
do_opcode_no_cyc:
    //printf("%04x: %02x\n",regs.pc-1,opcode);
    switch (opcode) {
        case 0x00: break; // nop (yay)

        // ld reg, #imm
        case 0x06: regs.b = read_PC(); break;
        case 0x0e: regs.c = read_PC(); break;
        case 0x16: regs.d = read_PC(); break;
        case 0x1e: regs.e = read_PC(); break;
        case 0x26: regs.h = read_PC(); break;
        case 0x2e: regs.l = read_PC(); break;
        case 0x36: writeZ80_EMU(REG_HL,read_PC()); break;
        case 0x3e: regs.a = read_PC(); break;

        // ld src, dst
        case 0x40: regs.b = regs.b; break;
        case 0x41: regs.b = regs.c; break;
        case 0x42: regs.b = regs.d; break;
        case 0x43: regs.b = regs.e; break;
        case 0x44: regs.b = regs.h; break;
        case 0x45: regs.b = regs.l; break;
        case 0x46: regs.b = readZ80_EMU(REG_HL); break;
        case 0x47: regs.b = regs.a; break;
        case 0x48: regs.c = regs.b; break;
        case 0x49: regs.c = regs.c; break;
        case 0x4a: regs.c = regs.d; break;
        case 0x4b: regs.c = regs.e; break;
        case 0x4c: regs.c = regs.h; break;
        case 0x4d: regs.c = regs.l; break;
        case 0x4e: regs.c = readZ80_EMU(REG_HL); break;
        case 0x4f: regs.c = regs.a; break;
        case 0x50: regs.d = regs.b; break;
        case 0x51: regs.d = regs.c; break;
        case 0x52: regs.d = regs.d; break;
        case 0x53: regs.d = regs.e; break;
        case 0x54: regs.d = regs.h; break;
        case 0x55: regs.d = regs.l; break;
        case 0x56: regs.d = readZ80_EMU(REG_HL); break;
        case 0x57: regs.d = regs.a; break;
        case 0x58: regs.e = regs.b; break;
        case 0x59: regs.e = regs.c; break;
        case 0x5a: regs.e = regs.d; break;
        case 0x5b: regs.e = regs.e; break;
        case 0x5c: regs.e = regs.h; break;
        case 0x5d: regs.e = regs.l; break;
        case 0x5e: regs.e = readZ80_EMU(REG_HL); break;
        case 0x5f: regs.e = regs.a; break;
        case 0x60: regs.h = regs.b; break;
        case 0x61: regs.h = regs.c; break;
        case 0x62: regs.h = regs.d; break;
        case 0x63: regs.h = regs.e; break;
        case 0x64: regs.h = regs.h; break;
        case 0x65: regs.h = regs.l; break;
        case 0x66: regs.h = readZ80_EMU(REG_HL); break;
        case 0x67: regs.h = regs.a; break;
        case 0x68: regs.l = regs.b; break;
        case 0x69: regs.l = regs.c; break;
        case 0x6a: regs.l = regs.d; break;
        case 0x6b: regs.l = regs.e; break;
        case 0x6c: regs.l = regs.h; break;
        case 0x6d: regs.l = regs.l; break;
        case 0x6e: regs.l = readZ80_EMU(REG_HL); break;
        case 0x6f: regs.l = regs.a; break;
        case 0x70: writeZ80_EMU(REG_HL,regs.b); break;
        case 0x71: writeZ80_EMU(REG_HL,regs.c); break;
        case 0x72: writeZ80_EMU(REG_HL,regs.d); break;
        case 0x73: writeZ80_EMU(REG_HL,regs.e); break;
        case 0x74: writeZ80_EMU(REG_HL,regs.h); break;
        case 0x75: writeZ80_EMU(REG_HL,regs.l); break;
        case 0x76: halt(); break; // halt
        case 0x77: writeZ80_EMU(REG_HL,regs.a); break;
        case 0x78: regs.a = regs.b; break;
        case 0x79: regs.a = regs.c; break;
        case 0x7a: regs.a = regs.d; break;
        case 0x7b: regs.a = regs.e; break;
        case 0x7c: regs.a = regs.h; break;
        case 0x7d: regs.a = regs.l; break;
        case 0x7e: regs.a = readZ80_EMU(REG_HL); break;
        case 0x7f: regs.a = regs.a; break;

        // jp cond
        case 0xc2: jp(!flags.z); break;
        case 0xca: jp(flags.z); break;
        case 0xd2: jp(!flags.c); break;
        case 0xda: jp(flags.c); break;
        case 0xe2: jp(!flags.p); break;
        case 0xea: jp(flags.p); break;
        case 0xf2: jp(!flags.s); break;
        case 0xfa: jp(flags.s); break;

        // call cond
        case 0xc4: call_cond(!flags.z,true); break;
        case 0xcc: call_cond(flags.z,true); break;
        case 0xd4: call_cond(!flags.c,true); break;
        case 0xdc: call_cond(flags.c,true); break;
        case 0xe4: call_cond(!flags.p,true); break;
        case 0xec: call_cond(flags.p,true); break;
        case 0xf4: call_cond(!flags.s,true); break;
        case 0xfc: call_cond(flags.s,true); break;
        // ret cond
        case 0xc0: ret(!flags.z,true); break;
        case 0xc8: ret(flags.z,true); break;
        case 0xd0: ret(!flags.c,true); break;
        case 0xd8: ret(flags.c,true); break;
        case 0xe0: ret(!flags.p,true); break;
        case 0xe8: ret(flags.p,true); break;
        case 0xf0: ret(!flags.s,true); break;
        case 0xf8: ret(flags.s,true); break;

        // add r8
        case 0x80: regs.a = add8(regs.a,regs.b,0); break;
        case 0x81: regs.a = add8(regs.a,regs.c,0); break;
        case 0x82: regs.a = add8(regs.a,regs.d,0); break;
        case 0x83: regs.a = add8(regs.a,regs.e,0); break;
        case 0x84: regs.a = add8(regs.a,regs.h,0); break;
        case 0x85: regs.a = add8(regs.a,regs.l,0); break;
        case 0x86: regs.a = add8(regs.a,readZ80_EMU(REG_HL),0); break;
        case 0x87: regs.a = add8(regs.a,regs.a,0); break;
        // adc r8
        case 0x88: regs.a = add8(regs.a,regs.b,flags.c); break;
        case 0x89: regs.a = add8(regs.a,regs.c,flags.c); break;
        case 0x8a: regs.a = add8(regs.a,regs.d,flags.c); break;
        case 0x8b: regs.a = add8(regs.a,regs.e,flags.c); break;
        case 0x8c: regs.a = add8(regs.a,regs.h,flags.c); break;
        case 0x8d: regs.a = add8(regs.a,regs.l,flags.c); break;
        case 0x8e: regs.a = add8(regs.a,readZ80_EMU(REG_HL),flags.c); break;
        case 0x8f: regs.a = add8(regs.a,regs.a,flags.c); break;
        // sub r8
        case 0x90: regs.a = sub8(regs.a,regs.b,0); break;
        case 0x91: regs.a = sub8(regs.a,regs.c,0); break;
        case 0x92: regs.a = sub8(regs.a,regs.d,0); break;
        case 0x93: regs.a = sub8(regs.a,regs.e,0); break;
        case 0x94: regs.a = sub8(regs.a,regs.h,0); break;
        case 0x95: regs.a = sub8(regs.a,regs.l,0); break;
        case 0x96: regs.a = sub8(regs.a,readZ80_EMU(REG_HL),0); break;
        case 0x97: regs.a = sub8(regs.a,regs.a,0); break;
        // sbc r8
        case 0x98: regs.a = sub8(regs.a,regs.b,flags.c); break;
        case 0x99: regs.a = sub8(regs.a,regs.c,flags.c); break;
        case 0x9a: regs.a = sub8(regs.a,regs.d,flags.c); break;
        case 0x9b: regs.a = sub8(regs.a,regs.e,flags.c); break;
        case 0x9c: regs.a = sub8(regs.a,regs.h,flags.c); break;
        case 0x9d: regs.a = sub8(regs.a,regs.l,flags.c); break;
        case 0x9e: regs.a = sub8(regs.a,readZ80_EMU(REG_HL),flags.c); break;
        case 0x9f: regs.a = sub8(regs.a,regs.a,flags.c); break;
        // and r8
        case 0xa0: regs.a = and8(regs.a,regs.b); break;
        case 0xa1: regs.a = and8(regs.a,regs.c); break;
        case 0xa2: regs.a = and8(regs.a,regs.d); break;
        case 0xa3: regs.a = and8(regs.a,regs.e); break;
        case 0xa4: regs.a = and8(regs.a,regs.h); break;
        case 0xa5: regs.a = and8(regs.a,regs.l); break;
        case 0xa6: regs.a = and8(regs.a,readZ80_EMU(REG_HL)); break;
        case 0xa7: regs.a = and8(regs.a,regs.a); break;
        // xor r8
        case 0xa8: regs.a = xor8(regs.a,regs.b); break;
        case 0xa9: regs.a = xor8(regs.a,regs.c); break;
        case 0xaa: regs.a = xor8(regs.a,regs.d); break;
        case 0xab: regs.a = xor8(regs.a,regs.e); break;
        case 0xac: regs.a = xor8(regs.a,regs.h); break;
        case 0xad: regs.a = xor8(regs.a,regs.l); break;
        case 0xae: regs.a = xor8(regs.a,readZ80_EMU(REG_HL)); break;
        case 0xaf: regs.a = xor8(regs.a,regs.a); break;
        // or r8
        case 0xb0: regs.a = or8(regs.a,regs.b); break;
        case 0xb1: regs.a = or8(regs.a,regs.c); break;
        case 0xb2: regs.a = or8(regs.a,regs.d); break;
        case 0xb3: regs.a = or8(regs.a,regs.e); break;
        case 0xb4: regs.a = or8(regs.a,regs.h); break;
        case 0xb5: regs.a = or8(regs.a,regs.l); break;
        case 0xb6: regs.a = or8(regs.a,readZ80_EMU(REG_HL)); break;
        case 0xb7: regs.a = or8(regs.a,regs.a); break;
        // cp r8
        case 0xb8: cp(regs.a,regs.b); break;
        case 0xb9: cp(regs.a,regs.c); break;
        case 0xba: cp(regs.a,regs.d); break;
        case 0xbb: cp(regs.a,regs.e); break;
        case 0xbc: cp(regs.a,regs.h); break;
        case 0xbd: cp(regs.a,regs.l); break;
        case 0xbe: cp(regs.a,readZ80_EMU(REG_HL)); break;
        case 0xbf: cp(regs.a,regs.a); break;
        // inc r8
        case 0x04: { bool flags_c = flags.c; regs.b = add8(regs.b,1,0); flags.c = flags_c; break; }
        case 0x0c: { bool flags_c = flags.c; regs.c = add8(regs.c,1,0); flags.c = flags_c; break; }
        case 0x14: { bool flags_c = flags.c; regs.d = add8(regs.d,1,0); flags.c = flags_c; break; }
        case 0x1c: { bool flags_c = flags.c; regs.e = add8(regs.e,1,0); flags.c = flags_c; break; }
        case 0x24: { bool flags_c = flags.c; regs.h = add8(regs.h,1,0); flags.c = flags_c; break; }
        case 0x2c: { bool flags_c = flags.c; regs.l = add8(regs.l,1,0); flags.c = flags_c; break; }
        case 0x34: { bool flags_c = flags.c; uint8_t val = readZ80_EMU(REG_HL); readZ80_EMU_dirty(REG_HL,1); writeZ80_EMU(REG_HL,add8(val,1,0)); flags.c = flags_c; break; }
        case 0x3c: { bool flags_c = flags.c; regs.a = add8(regs.a,1,0); flags.c = flags_c; break; }
        // dec r8
        case 0x05: { bool flags_c = flags.c; regs.b = sub8(regs.b,1,0); flags.c = flags_c; break; }
        case 0x0d: { bool flags_c = flags.c; regs.c = sub8(regs.c,1,0); flags.c = flags_c; break; }
        case 0x15: { bool flags_c = flags.c; regs.d = sub8(regs.d,1,0); flags.c = flags_c; break; }
        case 0x1d: { bool flags_c = flags.c; regs.e = sub8(regs.e,1,0); flags.c = flags_c; break; }
        case 0x25: { bool flags_c = flags.c; regs.h = sub8(regs.h,1,0); flags.c = flags_c; break; }
        case 0x2d: { bool flags_c = flags.c; regs.l = sub8(regs.l,1,0); flags.c = flags_c; break; }
        case 0x35: { bool flags_c = flags.c; uint8_t val = readZ80_EMU(REG_HL); readZ80_EMU_dirty(REG_HL,1); writeZ80_EMU(REG_HL,sub8(val,1,0)); flags.c = flags_c; break; }
        case 0x3d: { bool flags_c = flags.c; regs.a = sub8(regs.a,1,0); flags.c = flags_c; break; }
        // rst
        case 0xc7: call(0x00); break;
        case 0xcf: call(0x08); break;
        case 0xd7: call(0x10); break;
        case 0xdf: call(0x18); break;
        case 0xe7: call(0x20); break;
        case 0xef: call(0x28); break;
        case 0xf7: call(0x30); break;
        case 0xff: call(0x38); break;


        case 0xc1: write_r16(regs.b,regs.c,pop()); break; // pop bc
        case 0xd1: write_r16(regs.d,regs.e,pop()); break; // pop de
        case 0xe1: write_r16(regs.h,regs.l,pop()); break; // pop hl
        case 0xf1: write_AF(pop()); break; // pop af

        case 0xc5: push(REG_BC); break; // push bc
        case 0xd5: push(REG_DE); break; // push de
        case 0xe5: push(REG_HL); break; // push hl
        case 0xf5: push(read_AF()); break; // push af

        case 0xc3: jp(true); break; // jp imm
        case 0xcd: call_cond(true,false); break; // call imm
        case 0xc9: ret(true,false); break; // ret

        case 0xfe: cp(regs.a,read_PC()); break; // cp imm

        case 0x01: write_r16(regs.b,regs.c,read16()); break; // ld bc, nn
        case 0x11: write_r16(regs.d,regs.e,read16()); break; // ld de, nn
        case 0x21: write_r16(regs.h,regs.l,read16()); break; // ld hl, nn
        case 0x31: regs.sp = read16(); break; // ld sp, nn

        case 0x08: { // ex af, af'
            uint16_t temp = read_AF();
            write_AF(regs.as<<8|regs.fs);
            regs.as = temp>>8&0xff;
            regs.fs = temp&0xff;
            break;
        }

        case 0xd9: // exx
            swap(regs.b,regs.bs);
            swap(regs.c,regs.cs);
            swap(regs.d,regs.ds);
            swap(regs.e,regs.es);
            swap(regs.h,regs.hs);
            swap(regs.l,regs.ls);
            break;

        case 0xdd: { // IX prefix
            opcode = read_PC();
            add_cycles(1);
            if (opcode == 0xDD || opcode == 0xED || opcode == 0xFD) {
                inc_R;
                goto do_opcode;
            }

            if (opcode == 0xD9 || opcode == 0xEB) {
                inc_R;
                //add_cycles(IX_IY_cycle_lut[opcode]);
                goto do_opcode_no_cyc_R;
            }

            if (opcode == 0xCB) {
                index_cb(&regs.ix);
            } else {
                //add_cycles(IX_IY_cycle_lut[opcode]);
                int ind_ret = index_step(&regs.ix,opcode); 
                if (ind_ret == 1) {
                    printf("\nUNSUPPORTED IX OPCODE %04x: %02x\n",regs.pc-1,opcode);
                    ula.quit = 1; // exit(0);
                } else if (ind_ret == 2) {
                    goto do_opcode_no_cyc;
                }
            }
            break;
        }

        case 0xfd: { // IY prefix
            opcode = read_PC();
            add_cycles(1);
            if (opcode == 0xDD || opcode == 0xED || opcode == 0xFD) {
                inc_R;
                goto do_opcode;
            }

            if (opcode == 0xD9 || opcode == 0xEB) {
                //add_cycles(IX_IY_cycle_lut[opcode]);
                inc_R;
                goto do_opcode_no_cyc_R;
            }

            if (opcode == 0xCB) {
                index_cb(&regs.iy);
            } else {
                //add_cycles(IX_IY_cycle_lut[opcode]);
                int ind_ret = index_step(&regs.iy,opcode); 
                if (ind_ret == 1) {
                    printf("\nUNSUPPORTED IY OPCODE %04x: %02x\n",regs.pc-1,opcode);
                    ula.quit = 1; // exit(0);
                } else if (ind_ret == 2) {
                    goto do_opcode_no_cyc;
                }
            }
            break;
        }

        case 0x2A: { // ld hl, (nn)
            uint16_t addr = read16();
            regs.l = readZ80_EMU(addr);
            regs.h = readZ80_EMU(addr+1);
            break; 
        }

        case 0xf9: add_cycles(2); regs.sp = REG_HL; break; // ld sp, hl

        case 0x03: add_cycles(2); write_r16(regs.b,regs.c,REG_BC+1); break; // inc bc
        case 0x13: add_cycles(2); write_r16(regs.d,regs.e,REG_DE+1); break; // inc de
        case 0x23: add_cycles(2); write_r16(regs.h,regs.l,REG_HL+1); break; // inc hl
        case 0x33: add_cycles(2); regs.sp += 1; break; // inc sp

        case 0x0b: add_cycles(2); write_r16(regs.b,regs.c,REG_BC-1); break; // dec bc
        case 0x1b: add_cycles(2); write_r16(regs.d,regs.e,REG_DE-1); break; // dec de
        case 0x2b: add_cycles(2); write_r16(regs.h,regs.l,REG_HL-1); break; // dec hl
        case 0x3b: add_cycles(2); regs.sp -= 1; break; // dec sp

        case 0x32: writeZ80_EMU(read16(),regs.a); break; // ld (nn), a
        case 0x3a: regs.a = readZ80_EMU(read16()); break; // ld a, (nn)

        case 0x09: add_cycles(11-4); addhl(REG_BC); break; // add hl, bc
        case 0x19: add_cycles(11-4); addhl(REG_DE); break; // add hl, de
        case 0x29: add_cycles(11-4); addhl(REG_HL); break; // add hl, hl
        case 0x39: add_cycles(11-4); addhl(regs.sp); break; // add hl, sp

        case 0xeb: // ex de, hl
            swap(regs.d,regs.h);
            swap(regs.e,regs.l);
            break;

        case 0xed: // ED-prefixed opcodes
            EDprefix(read_PC());
            break;

        case 0x07: // rlca
            flags.h = 0;
            flags.n = 0;
            flags.c = regs.a>>7;
            regs.a = (regs.a>>7)|((regs.a&0x7f)<<1);
            setXYF(regs.a);
            break;

        case 0x0f: // rrca
            flags.h = 0;
            flags.n = 0;
            flags.c = regs.a&1;
            regs.a = ((regs.a&1)<<7)|(regs.a>>1);
            setXYF(regs.a);
            break;

        case 0x17: { // rla
            uint8_t carry = flags.c;
            flags.h = 0;
            flags.n = 0;
            flags.c = regs.a>>7;
            regs.a = carry|(regs.a<<1);
            setXYF(regs.a);
            break;
        }

        case 0x1f: { // rra
            uint8_t carry = flags.c;
            flags.h = 0;
            flags.n = 0;
            flags.c = regs.a&1;
            regs.a = (carry<<7)|(regs.a>>1);
            setXYF(regs.a);
            break;
        }

        // aluop imm
        case 0xc6: regs.a = add8(regs.a,read_PC(),0); break;
        case 0xce: regs.a = add8(regs.a,read_PC(),flags.c); break;
        case 0xd6: regs.a = sub8(regs.a,read_PC(),0); break;
        case 0xde: regs.a = sub8(regs.a,read_PC(),flags.c); break;
        case 0xe6: regs.a = and8(regs.a,read_PC()); break;
        case 0xee: regs.a = xor8(regs.a,read_PC()); break;
        case 0xf6: regs.a = or8(regs.a,read_PC()); break;

        case 0xf3: regs.iff1 = 0; regs.iff2 = 0; break; // di
        case 0xfb: regs.iff1 = 1; regs.iff2 = 1; break; // ei

        case 0x22: { // ld (nn), hl
            uint16_t addr = read16();
            writeZ80_EMU(addr, regs.l);
            writeZ80_EMU(addr+1, regs.h);
            break;
        }

        case 0x0a: regs.a = readZ80_EMU(REG_BC); break; // ld a, (bc)
        case 0x1a: regs.a = readZ80_EMU(REG_DE); break; // ld a, (de)
        case 0x02: writeZ80_EMU(REG_BC,regs.a); break; // ld (bc), a
        case 0x12: writeZ80_EMU(REG_DE,regs.a); break; // ld (de), a

        case 0xCB: cb_step(); break; // CB opcodes

        case 0x27: { // DAA
            uint8_t bcd = 0;

            if (((regs.a&15)>0x09) || flags.h)
                bcd += 0x06;

            if ((regs.a>0x99) || flags.c) {
                bcd += 0x60;
                flags.c = 1;
            }

            if (flags.n) {
                flags.h = flags.h && ((regs.a&15)<6);
                regs.a -= bcd;
            } else {
                flags.h = (regs.a&15)>9;
                regs.a += bcd;
            }

            flags.s = regs.a>>7&1;
            flags.p = parity(regs.a);
            flags.z = regs.a == 0;
            setXYF(regs.a);

            break;
        }

        case 0x2F: // CPL
            flags.h = 1;
            flags.n = 1;
            regs.a ^= 0xff; // negate A
            setXYF(regs.a);
            break;

        case 0x37: // SCF
            flags.c = 1;
            flags.h = 0;
            flags.n = 0;
            setXYF(regs.a);
            break;

        case 0x3F: // CCF
            flags.h = flags.c;
            flags.n = 0;
            flags.c ^= 1;
            setXYF(regs.a);
            break;

        case 0x10: { // DJNZ
            add_cycles(1);
            int8_t off = (int8_t)read_PC();
            if (--regs.b != 0) {
                readZ80_EMU_dirty(regs.pc-1,5);
                regs.pc += off;
            }
            break;
        }

        case 0x18: jr(true,false); break; // jr s8

        // jr cond
        case 0x20: jr(!flags.z,true); break;
        case 0x28: jr(flags.z,true); break;
        case 0x30: jr(!flags.c,true); break;
        case 0x38: jr(flags.c,true); break;

        case 0xd3: outZ80((regs.a<<8)|read_PC(),regs.a); break; // out (n), a

        case 0xe3: { // ex (sp), hl
            // pc:4,sp:3,sp+1:4,sp(write):3,sp+1(write):3,sp+1(write):1 x 2
            uint8_t temp = readZ80_EMU(regs.sp); 
            uint8_t temp_hi = readZ80_EMU(regs.sp+1); 
            add_cycles(1);
            writeZ80_EMU(regs.sp,regs.l);
            regs.l = temp;
            writeZ80_EMU(regs.sp+1,regs.h);
            writeZ80_EMU_dirty(regs.sp+1,regs.h,2);
            regs.h = temp_hi;
            break;
        }

        case 0xe9: regs.pc = REG_HL; break; // jp hl

        case 0xdb: regs.a = inZ80((regs.a<<8)|read_PC()); break; // in a, (n)

        default:
            printf("\nUNSUPPORTED OPCODE %04x: %02x\n",regs.pc-1,opcode);
            ula.quit = 1; // exit(0);
            break;        
    }

    // check for IRQ's later
    return opcode;
}

inline static void check_IRQ(uint8_t opcode) {
    if ((regs.has_int > -1) && regs.iff1 && (opcode != 0xfb)) {
        inc_R;
        regs.halt = 0;
        regs.iff1 = 0;
        add_cycles(7); regs.sp--;
        writeZ80_EMU(regs.sp--,regs.pc>>8);
        writeZ80_EMU(regs.sp,regs.pc&0xff);
        switch (regs.im&3) {
            case 0: // IM 0
                regs.pc = regs.has_int&0xff;
                break;
            case 1: // IM 1
                regs.pc = 0x38;
                break;
            case 2: { // IM 2
                // add_cycles(19-6-7);
                // TODO: FIX IM 2 R REGISTER
                uint16_t imm = (uint16_t)readZ80_EMU(((regs.i<<8)|regs.has_int));
                imm |= (uint16_t)readZ80_EMU(((regs.i<<8)|regs.has_int)+1)<<8;
                regs.pc = imm;
                break;
            }
            case 3: break; //ula.quit = 1; // exit(0); // IM 3
        }
        regs.has_int = -1; // NOTE: THIS LINE OF CODE IS SPECIFICALLY FOR ZX SPECTRUM VBLANK IRQS
    }
}

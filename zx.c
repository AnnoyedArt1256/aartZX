#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_render.h>
#include <SDL2/SDL_surface.h>
#include <SDL2/SDL_events.h>

#define SAMPLE_RATE 48000
#define BUFFER_SIZE (960*2) // 48000/50
#define CLAMP(x,y,z) ((x)>(z)?(z):((x)<(y)?(y):(x)))
#include "config.h"
#include "zx_128k_rom.h"

//typedef enum { false, true } bool; // booleans yay

struct window_bool {
    SHARED_BOOL memviewer;
    SHARED_BOOL imgui_debugger;
    SHARED_BOOL settings;
    SHARED_BOOL read;
    SHARED_BOOL write;
    SHARED_BOOL exec;
    uint32_t read_col;
    uint32_t write_col;
    uint32_t exec_col;
    SHARED_BOOL is_ym1;
    SHARED_BOOL is_ym2;
    uint8_t aypan;
    int AY_seperation;
    SHARED_BOOL cpu_regview;
    SHARED_BOOL contended;
    SHARED_BOOL disasm;
    float screen_scale;
    SHARED_BOOL do_event_viewer;
    SHARED_BOOL do_event_viewer_bitmap;
    SHARED_BOOL do_tsfm;
};

extern struct window_bool visible_windows;

#define WRITE_EVENT(num) \
    if (visible_windows.do_event_viewer) { \
        size_t scanline = ula.scanline-((ula.cycles-(ula.cycles%228))/228); \
        event_viewer[(ula.cycles%228)+(scanline*228)] = (num)+1; \
    }

uint8_t *event_viewer;

#include "ayumi.h"
#include "misc.h"
#include "cpu.h"

bool audio_paused;
float audio_volume;

struct regs_Struct *regs_rewind;
struct flags_Struct *flags_rewind;
struct ula_Struct *ula_rewind;
uint32_t *tap_pos_rewind;
uint8_t *mem_rewind;
uint32_t *mem_pos_rewind;
int *time_rewind;
int rewind_frame;
bool rewind_pressed;
bool pause_pressed;
bool paused;
bool actually_rewind;

#ifdef AY_TURBOSOUND_FM
extern void init_opn();
extern void write_opn(uint8_t addr, uint8_t val, uint8_t cs);
extern uint8_t read_opn(uint8_t addr, uint8_t cs);
extern void free_opn();
extern void OPN_advance_clock();
extern int OPN_get_sample();
extern uint8_t OPN_get_status(uint8_t cs);
#endif

#include "io.h"

uint32_t get_contended_cycles() {
    return ula.contended_stolen_cycles;
}

void reset_audio_buffer_and_unpause() {
    ula.audio_buffer_read = 0;
    ula.audio_buffer_write = BUFFER_SIZE;
    ula.time = SDL_GetTicks();
}

uint8_t get_ram_bank() {
    return ula.ram_bank;
}

uint8_t get_scr_rom_bank() {
    return (ula.gfx_sel?1:0)|(ula.rom_sel?2:0);
}

void callback(void *udata, uint8_t *stream, int len)
{
    SDL_memset(stream, 0, len);

    if (audio_paused) return;

    if (len > (BUFFER_SIZE<<1)) len = (BUFFER_SIZE<<1); // clamp length

    SDL_MixAudioFormat(
        stream, 
        (const Uint8 *)(&ula.audio_buffer[ula.audio_buffer_read]), 
        AUDIO_S16SYS, 
        len, 
        SDL_MIX_MAXVOLUME
    );

    ula.audio_buffer_read += len>>1;
    if (ula.audio_buffer_read >= (BUFFER_SIZE*4))
        ula.audio_buffer_read -= (BUFFER_SIZE*4);
}

FILE *tap;
int tap_file_size;
bool do_tap;

enum FILE_EXTENSIONS {
    TAP_FILE,
    SNA_FILE
};

enum FILE_EXTENSIONS file_ext;
int cur_time_rewind = 0;
int cur_mempos_rewind = 0;

int rle_comp(uint8_t **mem_buffer, int mem_pos, uint8_t *mem_in) {
    int rle_len = 1;
    for (int pos = 1; pos < 131072; pos++) { // 128k ram
        if (mem_in[pos-1] == mem_in[pos] && (pos != 131071)) {
            rle_len++;
        } else {
            // output
            uint8_t rle_val = mem_in[pos-1];
            while (rle_len > (255+2)) {
                (*mem_buffer)[mem_pos] = rle_val;
                mem_pos = (mem_pos+1)%REWIND_MEM;
                (*mem_buffer)[mem_pos] = rle_val;
                mem_pos = (mem_pos+1)%REWIND_MEM;
                (*mem_buffer)[mem_pos] = 255;
                mem_pos = (mem_pos+1)%REWIND_MEM;
                rle_len -= (255+2);
            }
            if (rle_len >= 2) {
                rle_len -= 2;
                (*mem_buffer)[mem_pos] = rle_val;
                mem_pos = (mem_pos+1)%REWIND_MEM;
                (*mem_buffer)[mem_pos] = rle_val;
                mem_pos = (mem_pos+1)%REWIND_MEM;
                (*mem_buffer)[mem_pos] = rle_len;
                mem_pos = (mem_pos+1)%REWIND_MEM;
            } else if (rle_len == 1) {
                (*mem_buffer)[mem_pos] = rle_val;
                mem_pos = (mem_pos+1)%REWIND_MEM;
            }
            rle_len = 1;
        }
    }
    return mem_pos;
}

int rle_decomp(uint8_t *mem_buffer, int mem_pos, uint8_t *mem_in) {
    int mem_len = 0;
    while (mem_len < 131072) {
        uint8_t first_byte = mem_buffer[mem_pos];
        mem_pos = (mem_pos+1)%REWIND_MEM;
        uint8_t extra_byte = mem_buffer[mem_pos];
        if (first_byte == extra_byte) {
            mem_pos = (mem_pos+1)%REWIND_MEM;
            int count = (int)(mem_buffer[mem_pos])+2;
            mem_pos = (mem_pos+1)%REWIND_MEM;
            for (int i = 0; i < count; i++) {
                (mem_in)[mem_len] = first_byte;
                mem_len++;
                if (mem_len >= 131072) return mem_pos;
            }
        } else {
            (mem_in)[mem_len] = first_byte;
            mem_len++;
            if (mem_len >= 131072) return mem_pos;      
        }
    }
    return mem_pos;
}

int last_no_press_mem_pos;
bool mem_pos_underflow;

void do_rewind() {
    if (ula.frame&1) {
        int ula_frame = ula.frame;
        int ula_time = ula.time;
        if (rewind_pressed) {
            // load FROM rewind buffer
            actually_rewind = false;
            if ((cur_time_rewind-time_rewind[rewind_frame]) < (REWIND_FRAMES) &&
                time_rewind[rewind_frame] > -1) {

                if ((mem_pos_rewind[rewind_frame] <= last_no_press_mem_pos) && mem_pos_underflow) {
                    goto end_rewind;
                }

                rewind_frame--;
                if (rewind_frame < 0)
                    rewind_frame += REWIND_FRAMES;


                if (((cur_time_rewind-time_rewind[rewind_frame]) >= (REWIND_FRAMES)) || 
                    (time_rewind[rewind_frame] == -1)) {
                    goto end_rewind;
                }


                if ((mem_pos_rewind[rewind_frame] <= last_no_press_mem_pos) && mem_pos_underflow) {
                    goto end_rewind;
                }


                actually_rewind = true;

                memcpy(&regs,&regs_rewind[rewind_frame],sizeof(regs));
                memcpy(&flags,&flags_rewind[rewind_frame],sizeof(flags));
                memcpy(&ula,&ula_rewind[rewind_frame],sizeof(ula));
                int old_mempos = cur_mempos_rewind;
                cur_mempos_rewind = rle_decomp(mem_rewind,mem_pos_rewind[rewind_frame],mem);
                if (old_mempos < cur_mempos_rewind) {
                    mem_pos_underflow = true;
                }
                if (do_tap) fseek(tap, (long)tap_pos_rewind[rewind_frame], SEEK_SET);
            }
        } else {
            // save to rewind buffer
            rewind_frame = (rewind_frame+1)%REWIND_FRAMES;
            memcpy(&regs_rewind[rewind_frame],&regs,sizeof(regs));
            memcpy(&flags_rewind[rewind_frame],&flags,sizeof(flags));
            memcpy(&ula_rewind[rewind_frame],&ula,sizeof(ula));
            if (do_tap)
                tap_pos_rewind[rewind_frame] = ftell(tap);
            else
                tap_pos_rewind[rewind_frame] = 0;
            time_rewind[rewind_frame] = cur_time_rewind;
            mem_pos_rewind[rewind_frame] = (uint32_t)cur_mempos_rewind;
            mem_pos_underflow = false;
            cur_mempos_rewind = rle_comp(&mem_rewind,cur_mempos_rewind,mem);
            last_no_press_mem_pos = cur_mempos_rewind;
            cur_time_rewind++;
        }
        end_rewind:
            ula.frame = ula_frame;
            ula.time = ula_time;
    }
}

bool has_ULA_quit() {
    return ula.quit;
}

SDL_Renderer* get_SDL_renderer();

// https://stackoverflow.com/questions/5309471/getting-file-extension-in-c#5309508
const char *get_filename_ext(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if(!dot || dot == filename) return "";
    return dot + 1;
}

static void read_SNA_header() {
    regs.i = fgetc(tap); // I
    regs.ls = fgetc(tap); // HL'
    regs.hs = fgetc(tap);
    regs.es = fgetc(tap); // DE'
    regs.ds = fgetc(tap);
    regs.cs = fgetc(tap); // BC'
    regs.bs = fgetc(tap);
    regs.fs = fgetc(tap); // AF'
    regs.as = fgetc(tap);

    regs.l = fgetc(tap); // HL
    regs.h = fgetc(tap);
    regs.e = fgetc(tap); // DE
    regs.d = fgetc(tap);
    regs.c = fgetc(tap); // BC
    regs.b = fgetc(tap);
    regs.iy = fgetc(tap); // IY
    regs.iy |= ((uint16_t)fgetc(tap))<<8;
    regs.ix = fgetc(tap); // IX
    regs.ix |= ((uint16_t)fgetc(tap))<<8;

    // IFF1 and IFF2
    uint8_t intr_bits = fgetc(tap);
    regs.iff1 = intr_bits&1;
    regs.iff2 = intr_bits>>2&1;

    regs.r = fgetc(tap); // R
    
    // AF
    uint16_t af_reg = fgetc(tap);
    af_reg |= ((uint16_t)fgetc(tap))<<8;
    write_AF(af_reg);

    regs.sp = fgetc(tap); // SP
    regs.sp |= ((uint16_t)fgetc(tap))<<8;

    regs.im = fgetc(tap)&3; // IM

    ula.ULA_FE = fgetc(tap)&7; // ULA border color
}

void read_SNA() {
    if (tap_file_size == 49179) {
        // 48k SNA file
        // read header
        read_SNA_header();
        // set 0x7FFD bank reg to be 48k compatible
        outZ80(0x7FFD,0x10);
        // write to memory
        for (size_t i = 0; i < 49152; i++) {
            writeZ80(i+0x4000,fgetc(tap));
        }
        EDprefix(0x45); // execute RETN
    } else if (tap_file_size == 131103 || tap_file_size == 147487) {
        // 128k SNA file
        read_SNA_header();
        // write to 0x7FFD
        long fpos = ftell(tap);
        fseek(tap, 49181L, SEEK_SET);
        outZ80(0x7FFD,fgetc(tap));
        fseek(tap, fpos, SEEK_SET);

        for (size_t i = 0; i < 16384; i++) mem[i|(5<<14)] = fgetc(tap); // write to bank 5
        for (size_t i = 0; i < 16384; i++) mem[i|(2<<14)] = fgetc(tap); // write to bank 2
        for (size_t i = 0; i < 16384; i++) mem[i|(ula.ram_bank<<14)] = fgetc(tap); // and bank N
        regs.pc = fgetc(tap); // PC
        regs.pc |= ((uint16_t)fgetc(tap))<<8;
        fgetc(tap); // 0x7FFD value (already written)
        fgetc(tap); // TR-DOS paged? (we don't have TR-DOS in the emulator yet)

        // check for which other banks to load
        static uint8_t banks_load[8];
        for (size_t i = 0; i < 8; i++) banks_load[i] = i;
        // eliminate bank 2 and 5 (since we loaded them in the 48k chunk anyway)
        banks_load[2] = 255;
        banks_load[5] = 255;
        // eliminate bank N (since again, we loaded it through the 48k chunk...)
        banks_load[ula.ram_bank] = -1;
        for (size_t i = 0; i < 8; i++) {
            if (banks_load[i] != 255) {
                uint8_t bank = banks_load[i];
                for (size_t p = 0; p < 16384; p++) {
                    mem[p|(bank<<14)] = fgetc(tap);
                }   
            }
        }
    } else {
        // fuck it, read it as .tap
        file_ext = TAP_FILE;
    }
}

void load_file(const char *file) {
    if (do_tap) fclose(tap);
    do_tap = true;
    tap_file_size = 0;
    tap = fopen(file,"rb");
    if (tap == NULL) {
        printf("ERROR: Failed to open file %s\n",file);
        do_tap = false;
        return;
    }

    const char *ext_str = get_filename_ext(file);
    file_ext = TAP_FILE;
    // if (strcmp(ext_str,"tap") == 0) file_ext = TAP_FILE;
    if (strcmp(ext_str,"sna") == 0) file_ext = SNA_FILE;

    // get file size
    fseek(tap, 0L, SEEK_END);
    tap_file_size = (int)ftell(tap);
    fseek(tap, 0L, SEEK_SET);

    if (file_ext == SNA_FILE) read_SNA();
}

void AY_set_pan(int pan_type);
void AY_set_clock(bool clock_mode);

void init_zx(int argc, char *argv[], bool init_files) {
    audio_paused = false;
    if (init_files) {
        do_tap = argc > 1;
        tap_file_size = 0;
        if (do_tap) {
            // NOTE: for people reading this source, modify any changes here in load_file too!!
            tap = fopen(argv[1],"rb");
            if (tap == NULL) {
                printf("ERROR: Failed to open file %s\n",argv[1]);
                do_tap = false;
            } else {
                const char *ext_str = get_filename_ext(argv[1]);
                file_ext = TAP_FILE;
                // if (strcmp(ext_str,"tap") == 0) file_ext = TAP_FILE;
                if (strcmp(ext_str,"sna") == 0) file_ext = SNA_FILE;

                // get file size
                fseek(tap, 0L, SEEK_END);
                tap_file_size = (int)ftell(tap);
                fseek(tap, 0L, SEEK_SET);
            }
        }

        zx_rom = (uint8_t*)malloc(32768*sizeof(uint8_t));

        if (argc > 2) { // read bios file from CLI arguments
            FILE *f = fopen(argv[2],"rb");
            assert(f);
            for (int i = 0; i < 32768; i++) {
                zx_rom[i] = fgetc(f);
            }
            fclose(f);
        } else {
            memcpy(zx_rom,zx_128k_rom,32768);
        }

    } else if (do_tap) {
        fseek(tap, 0L, SEEK_SET);
    }

    pixels = (uint32_t*)malloc(320*256*sizeof(uint32_t));
    mem_highlight = (uint32_t*)malloc(131072*sizeof(uint32_t));
    memset(mem_highlight,0,131072*sizeof(uint32_t));

    zx_rom_highlight = (uint32_t*)malloc(32768*sizeof(uint32_t));
    memset(zx_rom_highlight,0,32768*sizeof(uint32_t));

    // for rewind
    last_no_press_mem_pos = 0;
    rewind_frame = 0;

    regs_rewind = (struct regs_Struct*)malloc(REWIND_FRAMES*sizeof(struct regs_Struct));
    memset(regs_rewind,0,REWIND_FRAMES*sizeof(struct regs_Struct));

    flags_rewind = (struct flags_Struct*)malloc(REWIND_FRAMES*sizeof(struct flags_Struct));
    memset(flags_rewind,0,REWIND_FRAMES*sizeof(struct flags_Struct));

    ula_rewind = (struct ula_Struct*)malloc(REWIND_FRAMES*sizeof(struct ula_Struct));
    memset(ula_rewind,0,REWIND_FRAMES*sizeof(struct ula_Struct));

    tap_pos_rewind = (uint32_t*)malloc(REWIND_FRAMES*sizeof(uint32_t));
    memset(tap_pos_rewind,0,REWIND_FRAMES*sizeof(uint32_t));

    mem_rewind = (uint8_t*)malloc(REWIND_MEM*sizeof(uint8_t));
    memset(mem_rewind,0,REWIND_MEM*sizeof(uint8_t));

    time_rewind = (int*)malloc(REWIND_FRAMES*sizeof(int));
    memset(time_rewind,-1,REWIND_FRAMES*sizeof(int));

    mem_pos_rewind = (uint32_t*)malloc(REWIND_FRAMES*sizeof(uint32_t));
    memset(mem_pos_rewind,0,REWIND_FRAMES*sizeof(uint32_t));

    event_viewer = (uint8_t*)malloc(228*311*sizeof(uint8_t));
    memset(event_viewer,0,228*311*sizeof(uint8_t));
    
    cur_time_rewind = 0;
    cur_mempos_rewind = 0;

    rewind_pressed = false;
    actually_rewind = false;
    mem_pos_underflow = false;

    memset(&debug_pressed,0,sizeof(debug_pressed));

    // init Z80 registers
    memset(&regs,0,sizeof(regs));
    memset(&flags,0,sizeof(flags));
    memset(&ula,0,sizeof(ula));
    regs.pc = 0;
    regs.sp = 0xfffe;
    regs.has_int = -1;
    regs.im = 1;
    regs.iff1 = 0;
    regs.r = 255;
    memset(mem,0,sizeof(mem));
    //regs.cycles = 0;

    // init ULA
    ula.rom_sel = 0;
    ula.ram_bank = 0;
    ula.bank_reg = 0;
    ula.gfx_sel = 0;
    ula.scanline = 0;
    ula.ULA_FE = 0;
    ula.cycles_leftover = 0;
    memset(ula.key_matrix_buf,0,sizeof(ula.key_matrix_buf));

    // init ULA audio
    ula.audio_cycles = 0;
    #ifdef AY_TURBOSOUND_FM
        ula.audio_cycles_fm = 0;
    #endif
    ula.audio_buffer_read = 0;
    ula.audio_buffer_write = BUFFER_SIZE;
    ula.beeper_filter = 0;
    ula.audio_buffer_ind = 0;
    //audio_volume = 1;
    memset(ula.audio_buffer,0,sizeof(ula.audio_buffer));

    // init AY if enabled
    #ifdef AY_EMULATION
        #ifdef AY_TURBOSOUND
            memset(AY_regs,0,32);
            AY_ind = 0;
            if (!ayumi_configure(&AY_chip1, visible_windows.is_ym1, AY_CLOCK, SAMPLE_RATE)) {
                printf("ayumi_configure error (AY chip 1)\n");
            }
            if (!ayumi_configure(&AY_chip2, visible_windows.is_ym2, AY_CLOCK, SAMPLE_RATE)) {
                printf("ayumi_configure error (AY chip 2)\n");
            }
            // set stereo (to actually MONO)
            ayumi_set_pan(&AY_chip1, 0, 0, 0);
            ayumi_set_pan(&AY_chip1, 1, 0, 0);
            ayumi_set_pan(&AY_chip1, 2, 0, 0);
            ayumi_set_pan(&AY_chip2, 0, 0, 0);
            ayumi_set_pan(&AY_chip2, 1, 0, 0);
            ayumi_set_pan(&AY_chip2, 2, 0, 0);
        #else
            memset(AY_regs,0,16);
            AY_ind = 0;
            if (!ayumi_configure(&AY_chip1, 0, AY_CLOCK, SAMPLE_RATE)) {
                printf("ayumi_configure error (AY chip 1)\n");
            }
            // set stereo (to actually MONO)
            ayumi_set_pan(&AY_chip1, 0, 0, 0);
            ayumi_set_pan(&AY_chip1, 1, 0, 0);
            ayumi_set_pan(&AY_chip1, 2, 0, 0);
        #endif
        AY_set_pan(visible_windows.aypan);
        #ifdef AY_TURBOSOUND_FM
            init_opn();
            AY_set_clock(visible_windows.do_tsfm);
        #endif
    #endif

    memset(ula.key_matrix_buf,0,sizeof(ula.key_matrix_buf)); // clear key matrix buffer
    memset(ula.key_matrix,0,sizeof(ula.key_matrix)); // clear key matrix
    ula.quit = false;

    if (file_ext == SNA_FILE && do_tap) read_SNA();

    paused_prev = false;

    SDL_AudioSpec wanted;
    wanted.freq = SAMPLE_RATE;
    wanted.format = AUDIO_S16;
    wanted.channels = 2;
    wanted.samples = BUFFER_SIZE>>1;
    wanted.callback = callback;
    wanted.userdata = NULL;

    dev = SDL_OpenAudio(&wanted, NULL);
    SDL_PauseAudio(0);

    //ula.time = -69; //DL_GetTicks() + 20;
    reset_audio_buffer_and_unpause();
}

uint8_t op;

void do_oneop() {
    // HLE .tap loading yipeeeee
    if (regs.pc == 0x56C && ula.rom_sel && do_tap && file_ext == TAP_FILE) {
        // get tap block size (in 16-bit LE format)
        if (ftell(tap) >= (tap_file_size-1)) {
            flags.c = 1;
            ret(true,false);
        } else {
            int tap_size = fgetc(tap)-2;
            tap_size += fgetc(tap)<<8;
            // compare it with DE
            if (tap_size == REG_DE) {
                // now check if the first byte from the data block == A'
                // if not then clear carry and execute ret
                uint8_t data_block_byte = fgetc(tap);
                if (data_block_byte == regs.as) {
                    // (description found from r/emudev IIRC)
                    // if carry is set in F' then this is a LOAD, not a VERIFY
                    // so only if it is set I then read DE bytes from the tape,
                    // writing them starting at IX and then set carry in regular F
                    if (regs.fs&1) {
                        uint16_t chunk_size = REG_DE;
                        for (int i = 0; i < chunk_size; i++) {
                            writeZ80(regs.ix++,fgetc(tap));
                        }
                        fgetc(tap); // skip one byte
                        regs.d = 0;
                        regs.e = 0;
                        regs.h = 0;
                        regs.a = 0;
                        regs.l = 170;
                        flags.c = 1;
                        ret(true,false);
                    }
                } else {
                    // go back ONE byte
                    long pos = ftell(tap)-1;
                    fseek(tap, pos, SEEK_SET);
                    flags.c = 0;
                    ret(true,false);
                }
            } else {
                // go back 2 bytes
                long pos = ftell(tap)-2;
                fseek(tap, pos, SEEK_SET);
                flags.c = 0;
                ret(true,false);
            }
        }
        ula.audio_cycles_beeper = 8;
        goto skip_step;
    }

    ula.audio_cycles_beeper = 0;
    op = step();
skip_step:

    // ula video
    if (ula.cycles >= 228) advance_ULA();

    check_IRQ(op);

    // audio buffer
    int16_t beeper = (ula.ULA_FE>>4&1?((8192-1024)*2):0);
    ula.beeper_filter = ((ula.beeper_filter*(ula.audio_cycles_beeper*2))+beeper)/(ula.audio_cycles_beeper*2+1);

    #ifdef AY_TURBOSOUND_FM
        if (ula.audio_cycles_fm >= 291) { // (3500000/48000*4)
            ula.audio_cycles_fm -= 291;
            #ifdef AY_EMULATION
                OPN_advance_clock();
                OPN_advance_clock();
                OPN_advance_clock();
            #endif
        }
    #endif

    if (ula.audio_cycles >= 295) { // (3545400/48000*4)
        ula.audio_cycles -= 295;
        #ifdef AY_EMULATION
            // mix beeper with AY
            ayumi_process(&AY_chip1);
            ayumi_remove_dc(&AY_chip1);
            #ifdef AY_TURBOSOUND
                ayumi_process(&AY_chip2);
                ayumi_remove_dc(&AY_chip2);
            #endif

            #ifdef AY_TURBOSOUND_FM
            int opn_sample = OPN_get_sample(); 
            if (!visible_windows.do_tsfm) opn_sample = 0;

            ula.audio_buffer_temp[ula.audio_buffer_ind++] = 
                CLAMP((ula.beeper_filter+(int16_t)(AY_chip1.left*(8192-1024)*2.0)
                +(int16_t)(AY_chip2.left*(8192-1024)*2.0)+(opn_sample))*audio_volume,-32767,32767);
            ula.audio_buffer_temp[ula.audio_buffer_ind++] = 
                CLAMP((ula.beeper_filter+(int16_t)(AY_chip1.right*(8192-1024)*2.0)
                +(int16_t)(AY_chip2.right*(8192-1024)*2.0)+(opn_sample))*audio_volume,-32767,32767);
            #else
            #ifdef AY_TURBOSOUND
            ula.audio_buffer_temp[ula.audio_buffer_ind++] = 
                CLAMP((ula.beeper_filter+(int16_t)(AY_chip1.left*(8192-1024)*2.0)
                +(int16_t)(AY_chip2.left*(8192-1024)*2.0))*audio_volume,-32767,32767);
            ula.audio_buffer_temp[ula.audio_buffer_ind++] = 
                CLAMP((ula.beeper_filter+(int16_t)(AY_chip1.right*(8192-1024)*2.0)
                +(int16_t)(AY_chip2.right*(8192-1024)*2.0))*audio_volume,-32767,32767);
            #else
            ula.audio_buffer_temp[ula.audio_buffer_ind++] = 
                CLAMP((ula.beeper_filter+(int16_t)(AY_chip1.left*(8192-1024)*2.0))*audio_volume,-32767,32767);
            ula.audio_buffer_temp[ula.audio_buffer_ind++] = 
                CLAMP((ula.beeper_filter+(int16_t)(AY_chip1.right*(8192-1024)*2.0))*audio_volume,-32767,32767);
            #endif
            #endif
        #else
            ula.audio_buffer_temp[ula.audio_buffer_ind++] = ula.beeper_filter;
        #endif
        if (ula.audio_buffer_ind == BUFFER_SIZE) {
            ula.audio_buffer_ind = 0;
            memcpy(&ula.audio_buffer[ula.audio_buffer_write], 
                   ula.audio_buffer_temp,
                   BUFFER_SIZE<<1);

            ula.audio_buffer_write += BUFFER_SIZE;

            if (ula.audio_buffer_write >= (BUFFER_SIZE*4))
                ula.audio_buffer_write -= (BUFFER_SIZE*4);

            #ifdef AUDIO_SYNC
                int now = SDL_GetTicks();
                bool init_time = false;
                if (ula.time == -69) {
                    init_time = true;
                    ula.time = now + (1000/50);
                }
                if (ula.time > now) {
                    now = ula.time - now;
                    SDL_Delay(now);
                }
                if (!init_time) ula.time += 1000/50;
                do_rewind();
            #endif
        }
    }
}

void do_onescan() {
    int leftover_cyc = ula.cycles_leftover;
    ula.debug_cycles = 0;
    while (ula.debug_cycles < (228-leftover_cyc)) do_oneop(); // wait until cycle is approx
}

void main_zx() {
    if (paused) {
        if (debug_pressed.scan_pressed) {
            do_onescan();
        } else if (debug_pressed.step_pressed) {
            do_oneop();
        }
        debug_pressed.step_pressed = false;
        debug_pressed.scan_pressed = false;
        do_events();
        return;
    }
    ula.contended_stolen_cycles = 0;
    ula.did_frame = false;
    while (!ula.did_frame) {
        do_oneop();
    }
}

void deinit_zx(bool deinit_file) {
    if (deinit_file) free(zx_rom);
    free(pixels);
    if (do_tap && deinit_file) fclose(tap);
    SDL_PauseAudioDevice(dev, 1);
    SDL_CloseAudioDevice(dev);
    free(mem_highlight);
    free(zx_rom_highlight);
    // rewind
    free(regs_rewind);
    free(flags_rewind);
    free(ula_rewind);
    free(tap_pos_rewind);
    free(mem_rewind);
    free(time_rewind);
    free(mem_pos_rewind);
    free(event_viewer);
    #ifdef AY_TURBOSOUND_FM
        free_opn();
    #endif
}

void set_YM(bool is_ym, int chip) {
    // init AY if enabled
    #ifdef AY_EMULATION
        #ifdef AY_TURBOSOUND
            if (chip) {
                ayumi_set_ym(&AY_chip2, is_ym?1:0);
            } else {
                ayumi_set_ym(&AY_chip1, is_ym?1:0);
            }
        #else
            ayumi_set_ym(&AY_chip1, is_ym?1:0);
        #endif
    #endif
}

void AY_set_pan(int pan_type) {
    #ifdef AY_EMULATION
        float sep = ((float)(100-visible_windows.AY_seperation))/200.0f;
        switch (pan_type) {
            case 0:
            default: // Mono
                ayumi_set_pan(&AY_chip1, 0, 0.5, 0);
                ayumi_set_pan(&AY_chip1, 1, 0.5, 0);
                ayumi_set_pan(&AY_chip1, 2, 0.5, 0);
                break;

            case 1: // ABC Stereo
                ayumi_set_pan(&AY_chip1, 0, 0+sep, 0);
                ayumi_set_pan(&AY_chip1, 1, 0.5, 0);
                ayumi_set_pan(&AY_chip1, 2, 1-sep, 0);
                break;

            case 2: // ACB Stereo :flushed:
                ayumi_set_pan(&AY_chip1, 0, 0+sep, 0);
                ayumi_set_pan(&AY_chip1, 1, 1-sep, 0);
                ayumi_set_pan(&AY_chip1, 2, 0.5, 0);
                break;
        }
        #ifdef AY_TURBOSOUND
            switch (pan_type) {
                case 0:
                default: // Mono
                    ayumi_set_pan(&AY_chip2, 0, 0.5, 0);
                    ayumi_set_pan(&AY_chip2, 1, 0.5, 0);
                    ayumi_set_pan(&AY_chip2, 2, 0.5, 0);
                    break;
    
                case 1: // ABC Stereo
                    ayumi_set_pan(&AY_chip2, 0, 0+sep, 0);
                    ayumi_set_pan(&AY_chip2, 1, 0.5, 0);
                    ayumi_set_pan(&AY_chip2, 2, 1-sep, 0);
                    break;
    
                case 2: // ACB Stereo :flushed:
                    ayumi_set_pan(&AY_chip2, 0, 0+sep, 0);
                    ayumi_set_pan(&AY_chip2, 1, 1-sep, 0);
                    ayumi_set_pan(&AY_chip2, 2, 0.5, 0);
                    break;
            }
        #endif
    #endif
}

void AY_set_clock(bool clock_mode) {
    #ifdef AY_EMULATION
        #ifdef AY_TURBOSOUND
            ayumi_set_clock(&AY_chip2, clock_mode?TSFM_CLOCK:AY_CLOCK, SAMPLE_RATE);
            ayumi_set_clock(&AY_chip1, clock_mode?TSFM_CLOCK:AY_CLOCK, SAMPLE_RATE);
        #else
            //ayumi_set_clock(&AY_chip1, AY_CLOCK, SAMPLE_RATE);
        #endif
    #endif
}
//#define VIDEO_SYNC 
#define AUDIO_SYNC

#define AY_EMULATION
#define AY_TURBOSOUND
#define AY_CLOCK 1772700 // 1.772 mHz ((228*312*50)/2)

#define REWIND_FRAMES (512)
#define REWIND_MEM (131072*128)

#define SHARED_BOOL uint8_t // for interfacing between C and C++ code
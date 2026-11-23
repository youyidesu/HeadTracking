/**
 * @file    pitches.h
 * @brief   音名 -> 频率(Hz) 整数宏，供 STM32 定时器 PWM 使用
 * @note    由 tools/gen_pitches.py 生成。A4 = 440 Hz，12 平均律，四舍五入取整。
 *          取值范围 A0(28Hz) .. C8(4186Hz)，全部能放进 uint16_t。
 *          0 表示休止符（不发声）。
 */
#ifndef __PITCHES_H
#define __PITCHES_H

#include <stdint.h>

#define NOTE_REST   0u   /* 休止符 */

#define A4_HZ       440u /* 定音参考，改这里可整体调音 */

#define NOTE_C0      16u    /*   16.35 Hz */
#define NOTE_CS0     17u    /*   17.32 Hz */
#define NOTE_D0      18u    /*   18.35 Hz */
#define NOTE_DS0     19u    /*   19.45 Hz */
#define NOTE_E0      21u    /*   20.60 Hz */
#define NOTE_F0      22u    /*   21.83 Hz */
#define NOTE_FS0     23u    /*   23.12 Hz */
#define NOTE_G0      24u    /*   24.50 Hz */
#define NOTE_GS0     26u    /*   25.96 Hz */
#define NOTE_A0      28u    /*   27.50 Hz */
#define NOTE_AS0     29u    /*   29.14 Hz */
#define NOTE_B0      31u    /*   30.87 Hz */

#define NOTE_C1      33u    /*   32.70 Hz */
#define NOTE_CS1     35u    /*   34.65 Hz */
#define NOTE_D1      37u    /*   36.71 Hz */
#define NOTE_DS1     39u    /*   38.89 Hz */
#define NOTE_E1      41u    /*   41.20 Hz */
#define NOTE_F1      44u    /*   43.65 Hz */
#define NOTE_FS1     46u    /*   46.25 Hz */
#define NOTE_G1      49u    /*   49.00 Hz */
#define NOTE_GS1     52u    /*   51.91 Hz */
#define NOTE_A1      55u    /*   55.00 Hz */
#define NOTE_AS1     58u    /*   58.27 Hz */
#define NOTE_B1      62u    /*   61.74 Hz */

#define NOTE_C2      65u    /*   65.41 Hz */
#define NOTE_CS2     69u    /*   69.30 Hz */
#define NOTE_D2      73u    /*   73.42 Hz */
#define NOTE_DS2     78u    /*   77.78 Hz */
#define NOTE_E2      82u    /*   82.41 Hz */
#define NOTE_F2      87u    /*   87.31 Hz */
#define NOTE_FS2     92u    /*   92.50 Hz */
#define NOTE_G2      98u    /*   98.00 Hz */
#define NOTE_GS2    104u    /*  103.83 Hz */
#define NOTE_A2     110u    /*  110.00 Hz */
#define NOTE_AS2    117u    /*  116.54 Hz */
#define NOTE_B2     123u    /*  123.47 Hz */

#define NOTE_C3     131u    /*  130.81 Hz */
#define NOTE_CS3    139u    /*  138.59 Hz */
#define NOTE_D3     147u    /*  146.83 Hz */
#define NOTE_DS3    156u    /*  155.56 Hz */
#define NOTE_E3     165u    /*  164.81 Hz */
#define NOTE_F3     175u    /*  174.61 Hz */
#define NOTE_FS3    185u    /*  185.00 Hz */
#define NOTE_G3     196u    /*  196.00 Hz */
#define NOTE_GS3    208u    /*  207.65 Hz */
#define NOTE_A3     220u    /*  220.00 Hz */
#define NOTE_AS3    233u    /*  233.08 Hz */
#define NOTE_B3     247u    /*  246.94 Hz */

#define NOTE_C4     262u    /*  261.63 Hz */
#define NOTE_CS4    277u    /*  277.18 Hz */
#define NOTE_D4     294u    /*  293.66 Hz */
#define NOTE_DS4    311u    /*  311.13 Hz */
#define NOTE_E4     330u    /*  329.63 Hz */
#define NOTE_F4     349u    /*  349.23 Hz */
#define NOTE_FS4    370u    /*  369.99 Hz */
#define NOTE_G4     392u    /*  392.00 Hz */
#define NOTE_GS4    415u    /*  415.30 Hz */
#define NOTE_A4     440u    /*  440.00 Hz */
#define NOTE_AS4    466u    /*  466.16 Hz */
#define NOTE_B4     494u    /*  493.88 Hz */

#define NOTE_C5     523u    /*  523.25 Hz */
#define NOTE_CS5    554u    /*  554.37 Hz */
#define NOTE_D5     587u    /*  587.33 Hz */
#define NOTE_DS5    622u    /*  622.25 Hz */
#define NOTE_E5     659u    /*  659.26 Hz */
#define NOTE_F5     698u    /*  698.46 Hz */
#define NOTE_FS5    740u    /*  739.99 Hz */
#define NOTE_G5     784u    /*  783.99 Hz */
#define NOTE_GS5    831u    /*  830.61 Hz */
#define NOTE_A5     880u    /*  880.00 Hz */
#define NOTE_AS5    932u    /*  932.33 Hz */
#define NOTE_B5     988u    /*  987.77 Hz */

#define NOTE_C6    1047u    /* 1046.50 Hz */
#define NOTE_CS6   1109u    /* 1108.73 Hz */
#define NOTE_D6    1175u    /* 1174.66 Hz */
#define NOTE_DS6   1245u    /* 1244.51 Hz */
#define NOTE_E6    1319u    /* 1318.51 Hz */
#define NOTE_F6    1397u    /* 1396.91 Hz */
#define NOTE_FS6   1480u    /* 1479.98 Hz */
#define NOTE_G6    1568u    /* 1567.98 Hz */
#define NOTE_GS6   1661u    /* 1661.22 Hz */
#define NOTE_A6    1760u    /* 1760.00 Hz */
#define NOTE_AS6   1865u    /* 1864.66 Hz */
#define NOTE_B6    1976u    /* 1975.53 Hz */

#define NOTE_C7    2093u    /* 2093.00 Hz */
#define NOTE_CS7   2217u    /* 2217.46 Hz */
#define NOTE_D7    2349u    /* 2349.32 Hz */
#define NOTE_DS7   2489u    /* 2489.02 Hz */
#define NOTE_E7    2637u    /* 2637.02 Hz */
#define NOTE_F7    2794u    /* 2793.83 Hz */
#define NOTE_FS7   2960u    /* 2959.96 Hz */
#define NOTE_G7    3136u    /* 3135.96 Hz */
#define NOTE_GS7   3322u    /* 3322.44 Hz */
#define NOTE_A7    3520u    /* 3520.00 Hz */
#define NOTE_AS7   3729u    /* 3729.31 Hz */
#define NOTE_B7    3951u    /* 3951.07 Hz */

#define NOTE_C8    4186u    /* 4186.01 Hz */
#define NOTE_CS8   4435u    /* 4434.92 Hz */
#define NOTE_D8    4699u    /* 4698.64 Hz */
#define NOTE_DS8   4978u    /* 4978.03 Hz */
#define NOTE_E8    5274u    /* 5274.04 Hz */
#define NOTE_F8    5588u    /* 5587.65 Hz */
#define NOTE_FS8   5920u    /* 5919.91 Hz */
#define NOTE_G8    6272u    /* 6271.93 Hz */
#define NOTE_GS8   6645u    /* 6644.88 Hz */
#define NOTE_A8    7040u    /* 7040.00 Hz */
#define NOTE_AS8   7459u    /* 7458.62 Hz */
#define NOTE_B8    7902u    /* 7902.13 Hz */

/* ---- 常用时值（ms，按 120 BPM 的 4/4 拍；只在你手写曲子时用得上）---- */
#define DUR_WHOLE      2000u
#define DUR_HALF       1000u
#define DUR_QUARTER     500u
#define DUR_EIGHTH      250u
#define DUR_SIXTEENTH   125u

#endif /* __PITCHES_H */

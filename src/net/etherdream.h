#pragma once
/** etherdream.h — EtherDream DAC network-Emulation
 *  Compatible with QLC+, Pangolin, Mamba Black, LaserBoy, Shownet
 */
namespace etherdream {
void init();
void task(void*);
bool isConnected();
bool isPlaying();
}

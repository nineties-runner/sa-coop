#pragma once
#include <cstdint>

namespace coop::radio {

constexpr int8_t RADIO_USER_TRACKS = 12;
constexpr int8_t RADIO_OFF = 13;

bool CarRadio();
int8_t RequestedStation();
int8_t ActiveStation();
void TuneToUserTracks();
bool ShouldMuteNativeRadio();
bool IsDriverOnUserTracks();
bool IsPassengerOnUserTracks();
float GameRadioVolume();

}  // namespace coop::radio

/*
 * Independent X55 RIFF/IMA and SI3 decoders.
 *
 * IMA quantizer: DATWG Recommended Practices, revision 3 (1992), pp. 28-32,
 * https://www.cs.columbia.edu/~hgs/audio/dvi/IMA_ADPCM.pdf
 * RIFF block layout and SI3 grammar: qualified firmware packet contracts.
 * SI3 oscillator shape, pitch scale, gain and release are emulator policy,
 * not recovered audio-part hardware semantics.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/audio/x55_codec.h"

static size_t block_frames(unsigned channels, size_t bytes)
{
    size_t group = 4 * channels;

    if (!group || bytes < group || bytes % group) {
        return 0;
    }
    return 1 + (bytes - group) * 2 / channels;
}

bool x55_wave_parse(const uint8_t *bytes, size_t size, X55Wave *wave)
{
    X55Wave result = { 0 };
    unsigned seen = 0;
    size_t end;

    if (!bytes || !wave || size < 12 || memcmp(bytes, "RIFF", 4) ||
        memcmp(bytes + 8, "WAVE", 4)) {
        return false;
    }
    end = (uint64_t)ldl_le_p(bytes + 4) + 8;
    if (end < 12 || end > size) {
        return false;
    }
    result.file_size = end;
    for (size_t pos = 12; pos < end;) {
        uint32_t length;
        const uint8_t *data;
        size_t padded;
        unsigned flag = 0;

        if (end - pos < 8) {
            return false;
        }
        length = ldl_le_p(bytes + pos + 4);
        padded = (uint64_t)length + (length & 1);
        if (padded > end - pos - 8) {
            return false;
        }
        data = bytes + pos + 8;
        if (!memcmp(bytes + pos, "fmt ", 4)) {
            flag = 1;
            if (length < 20 || lduw_le_p(data) != 0x11 ||
                lduw_le_p(data + 14) != 4 ||
                lduw_le_p(data + 16) < 2 ||
                lduw_le_p(data + 16) > length - 18) {
                return false;
            }
            result.channels = lduw_le_p(data + 2);
            result.rate = ldl_le_p(data + 4);
            result.block_size = lduw_le_p(data + 12);
            result.block_frames = lduw_le_p(data + 18);
        } else if (!memcmp(bytes + pos, "fact", 4)) {
            flag = 2;
            if (length < 4) {
                return false;
            }
            result.frames = ldl_le_p(data);
        } else if (!memcmp(bytes + pos, "data", 4)) {
            flag = 4;
            result.data_offset = pos + 8;
            result.data_size = length;
        }
        if (seen & flag) {
            return false;
        }
        seen |= flag;
        pos += 8 + padded;
    }
    if (seen != 7 || result.channels < 1 || result.channels > 2 ||
        !result.rate || result.rate > 192000 || !result.frames ||
        !result.data_size || result.block_size > X55_IMA_BLOCK_MAX ||
        !result.block_frames ||
        block_frames(result.channels, result.block_size) !=
        result.block_frames) {
        return false;
    }
    size_t tail = result.data_size % result.block_size;
    uint64_t frames = (uint64_t)(result.data_size / result.block_size) *
                      result.block_frames;
    size_t last = tail ? block_frames(result.channels, tail) :
                        result.block_frames;

    if (!last) {
        return false;
    }
    frames += tail ? last : 0;
    /* fact may trim padding in the last block, not discard whole blocks. */
    if (result.frames > frames || result.frames <= frames - last) {
        return false;
    }
    *wave = result;
    return true;
}

static const uint16_t ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
};

int x55_ima_decode(const X55Wave *wave, const uint8_t *block, size_t size,
                   int16_t *samples, size_t capacity)
{
    int predictor[2], index[2];
    size_t frames;
    unsigned channels = wave->channels;
    static const int8_t changes[] = { -1, -1, -1, -1, 2, 4, 6, 8 };

    if (channels < 1 || channels > 2 || size > wave->block_size ||
        wave->block_size > X55_IMA_BLOCK_MAX ||
        block_frames(channels, wave->block_size) != wave->block_frames) {
        return -1;
    }
    frames = block_frames(channels, size);
    if (!frames || frames * channels > capacity) {
        return -1;
    }
    for (unsigned ch = 0; ch < channels; ch++) {
        predictor[ch] = (int16_t)lduw_le_p(block + ch * 4);
        index[ch] = block[ch * 4 + 2];
        if (index[ch] > 88 || block[ch * 4 + 3]) {
            return -1;
        }
        samples[ch] = predictor[ch];
    }
    for (size_t frame = 1; frame < frames; frame++) {
        size_t group = (frame - 1) / 8;
        unsigned nibble = (frame - 1) % 8;

        for (unsigned ch = 0; ch < channels; ch++) {
            size_t offset = 4 * channels + group * 4 * channels + ch * 4;
            unsigned code = (block[offset + nibble / 2] >>
                             (4 * (nibble & 1))) & 15;
            int step = ima_steps[index[ch]];
            int delta = step >> 3;

            for (unsigned bit = 0; bit < 3; bit++) {
                if (code & (1 << bit)) {
                    delta += step >> (2 - bit);
                }
            }
            predictor[ch] = CLAMP(predictor[ch] +
                                  (code & 8 ? -delta : delta), -32768, 32767);
            index[ch] = CLAMP(index[ch] + changes[code & 7], 0, 88);
            samples[frame * channels + ch] = predictor[ch];
        }
    }
    return frames * channels;
}

static int16_t si3_sample(X55SI3 *state)
{
    int64_t sum = 0;

    for (unsigned slot = 0; slot < X55_SI3_VOICES; slot++) {
        X55SI3Voice *voice = &state->voices[slot];
        unsigned phase, folded;
        int64_t gain;

        if (!voice->active) {
            continue;
        }
        phase = voice->phase >> 16;
        folded = MIN(phase, 65535 - phase);
        gain = voice->gain;
        if (voice->release) {
            gain = gain * voice->release / 1024;
        }
        sum += ((int64_t)folded * 4 - 65534) * gain / 65534;
        voice->phase += voice->increment;
        if (voice->release && !--voice->release) {
            voice->active = false;
        }
    }
    return CLAMP(sum, -32768, 32767);
}

int x55_si3_packet_profile(X55SI3 *state, uint32_t clock_hz,
                   const uint8_t *packet, size_t size,
                   int16_t *samples, size_t capacity, bool recovered)
{
    size_t used = 0;

    if (!state || !samples || !clock_hz) {
        return -1;
    }
    if (!packet || !size || (size & 1)) {
        goto malformed;
    }
    size_t waits = 0;
    if (size > 128) {
        goto malformed;
    }
    for (size_t pos = 0; pos < size;) {
        unsigned op = lduw_le_p(packet + pos), family = op >> 8;
        size_t length = 2;
        if (op == 0xc1cc) {
            waits++;
        } else if (family == 0x11 || family == 0x91) {
            if (size - pos < 4) {
                goto malformed;
            }
            unsigned kind = packet[pos + 3] >> 5;
            if (recovered && (kind == 2 || kind == 3)) {
                goto malformed;
            }
            length = recovered && kind >= 4 ? 26 : 16;
        } else if (recovered && (family & 0x70) == 0x20) {
            length = 6;
        } else if (family != 0x01 && family != 0x81) {
            goto malformed;
        }
        if (size - pos < length) {
            goto malformed;
        }
        pos += length;
    }
    if ((state->remainder + waits * 120000ull * X55_SI3_RATE) / clock_hz > capacity) {
        goto malformed;
    }
    while (size) {
        unsigned op = lduw_le_p(packet);
        unsigned family = op >> 8;
        X55SI3Voice *voice = &state->voices[(op >> 4) & 15];
        size_t consumed = 2;

        if (op == 0xc1cc) {
            uint64_t time = state->remainder + 120000ull * X55_SI3_RATE;
            size_t count = time / clock_hz;

            state->remainder = time % clock_hz;
            if (count > capacity - used) {
                goto malformed;
            }
            for (size_t i = 0; i < count; i++) {
                samples[used++] = si3_sample(state);
            }
            state->quanta++;
        } else if (family == 0x11 || family == 0x91) {
            unsigned pitch, octave;

            if (size < 16) {
                goto malformed;
            }
            unsigned packed = lduw_le_p(packet + 4);
            pitch = packed >> (recovered ? 6 : 4);
            octave = (packed >> 2) & (recovered ? 15 : 3);
            uint64_t frequency = ((uint64_t)pitch << octave) * 190;
            uint64_t denominator = X55_SI3_RATE * 1000ull * (recovered ? 4 : 1);
            consumed = recovered && (packet[3] >> 5) >= 4 ? 26 : 16;
            *voice = (X55SI3Voice) {
                .active = true,
                .gain = (packet[3] & (recovered ? 31 : 255)) * 57,
                .increment = ((frequency % denominator) << 32) / denominator,
                .parameter_length = consumed - 2,
            };
            memcpy(voice->parameters, packet + 2, consumed - 2);
            state->starts[family == 0x91]++;

        } else if (family == 0x01 || family == 0x81) {
            if (voice->active) {
                voice->release = 1024;
            }
            state->stops++;
        } else if (recovered && (family & 0x70) == 0x20) {
            memcpy(state->last_control, packet, 6);
            state->controls++;
            consumed = 6;
        } else {
            goto malformed;
        }
        size -= consumed;
        packet += consumed;
    }
    return used;
malformed:
    state->malformed++;
    return -1;
}

int x55_ima_packet(X55IMA *state, const uint8_t *packet, size_t size,
                   int16_t *samples, size_t capacity)
{
    static const int changes[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    X55IMA next = *state;
    size_t used = 0;

    if (size < 6 || (size & 1) || lduw_le_p(packet) != 0xadbc) {
        return -1;
    }
    unsigned marker = lduw_le_p(packet + 2);
    size_t payload = size - 4;
    size_t header = marker * 2u;
    if (marker != 0xffff &&
        (payload < 4 || header > payload - 4 || packet[4 + header + 2] > 88 ||
         packet[4 + header + 3])) {
        return -1;
    }
    if ((!next.have_header && marker != 0) ||
        capacity < payload * 2 - (marker == 0xffff ? 0 : 7)) {
        return -1;
    }
    for (size_t pos = 0; pos < payload;) {
        if (marker != 0xffff && pos == header) {
            next.predictor = (int16_t)lduw_le_p(packet + 4 + pos);
            next.index = packet[4 + pos + 2];
            next.have_header = true;
            samples[used++] = next.predictor;
            pos += 4;
            continue;
        }
        unsigned byte = packet[4 + pos++];
        for (unsigned nibble = 0; nibble < 2; nibble++, byte >>= 4) {
            unsigned code = byte & 15;
            int step = ima_steps[next.index];
            int delta = step >> 3;
            for (unsigned bit = 0; bit < 3; bit++) {
                if (code & (1 << bit)) {
                    delta += step >> (2 - bit);
                }
            }
            next.predictor = CLAMP(next.predictor +
                                   (code & 8 ? -delta : delta), -32768, 32767);
            next.index = CLAMP(next.index + changes[code & 7], 0, 88);
            samples[used++] = next.predictor;
        }
    }
    *state = next;
    return used;
}

int x55_si3_packet(X55SI3 *state, uint32_t clock_hz,
    const uint8_t *packet, size_t size, int16_t *samples, size_t capacity)
{
    return x55_si3_packet_profile(state, clock_hz, packet, size, samples,
                                  capacity, true);
}

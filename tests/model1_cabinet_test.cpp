#include "model1_cabinet.h"

#include <cstdio>

int main() {
    model1_cabinet::nvram_image image{};
    image[0] = 'S';
    image[1] = 'E';
    image[2] = 'G';
    image[3] = 'A';
    image[4] = 0x1c;
    image[5] = 0x82;
    image[6] = 0x01;
    image[8] = 0x88;
    image[9] = 0x9a;
    image[10] = 0xff;
    image[11] = 0x01;
    image[17] = 0x01;
    image[18] = 0x01;
    image[19] = 0x01;
    image[21] = 0x01;
    image[22] = 0x01;
    image[23] = 0xff;
    image[24] = 0xff;
    image[25] = 0xff;
    image[26] = 0x01;
    image[27] = 0x02;
    image[32] = 0x01;
    image[124] = 0x02;

    if (model1_cabinet::checksum(image) != 0x9a88 ||
        !model1_cabinet::checksum_valid(image) ||
        !model1_cabinet::attract_sound_enabled(image)) {
        std::fprintf(stderr, "Factory Formula cabinet image was rejected\n");
        return 1;
    }

    model1_cabinet::set_attract_sound(image, false);
    if (model1_cabinet::attract_sound_enabled(image) ||
        !model1_cabinet::checksum_valid(image)) {
        std::fprintf(stderr, "Attract-sound OFF did not update the CRC\n");
        return 2;
    }

    model1_cabinet::set_attract_sound(image, true);
    if (!model1_cabinet::attract_sound_enabled(image) ||
        !model1_cabinet::checksum_valid(image) ||
        model1_cabinet::checksum(image) != 0x9a88) {
        std::fprintf(stderr, "Attract-sound ON did not restore factory data\n");
        return 3;
    }
    return 0;
}

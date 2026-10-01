#include <gtest/gtest.h>

#include "audio/opus/SilkNlsf.hpp"
#include "audio/opus/SilkTables.hpp"

#include <cstdint>
#include <vector>

namespace {

// Vectors generated with the libopus reference (v1.3.1).

const int8_t kIdx0_0[11] = {24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, };
const int16_t kNlsf0_0[10] = {3840, 4864, 7168, 11392, 15104, 16512, 20224, 22784, 25600, 29568, };

const int8_t kIdx0_1[11] = {29, 4, -4, 4, -4, 4, -4, 4, -4, 4, -4, };
const int16_t kNlsf0_1[10] = {5070, 5073, 11959, 11962, 18482, 18485, 22253, 22256, 26177, 26180, };

const int8_t kIdx0_2[11] = {23, 1, 4, -3, 3, -3, 4, -1, 2, -2, 4, };
const int16_t kNlsf0_2[10] = {5069, 5072, 5078, 15945, 15948, 21553, 21557, 24989, 26098, 32307, };

const int8_t kIdx0_3[11] = {25, 0, 0, 3, 1, 1, -2, 0, -3, 1, 0, };
const int16_t kNlsf0_3[10] = {3208, 4456, 9358, 9361, 11013, 11136, 16829, 17440, 25982, 28416, };

const int8_t kIdx0_4[11] = {27, 4, 3, -4, 2, -4, 1, 2, 4, 3, -1, };
const int16_t kNlsf0_4[10] = {7249, 7252, 7258, 13780, 13783, 24528, 29032, 30699, 30702, 30705, };

const int8_t kIdx0_5[11] = {31, 1, -1, -3, 2, 1, 1, 0, 0, 4, -3, };
const int16_t kNlsf0_5[10] = {4418, 4421, 5764, 13396, 14883, 16303, 20501, 23901, 27510, 27513, };

const int8_t kIdx0_6[11] = {21, 2, 4, 4, 4, 4, 2, 0, -2, -2, 2, };
const int16_t kNlsf0_6[10] = {7152, 8058, 18525, 19167, 19171, 19177, 19181, 19787, 24741, 30568, };

const int8_t kIdx0_7[11] = {1, -1, 4, -2, -3, -2, 1, -2, -1, -4, 4, };
const int16_t kNlsf0_7[10] = {1733, 3819, 3825, 4388, 9806, 14905, 15416, 20088, 23841, 32307, };

const int8_t kIdx0_8[11] = {2, 1, 3, -2, -3, 4, -2, -1, 3, -1, 1, };
const int16_t kNlsf0_8[10] = {4810, 6251, 6257, 9870, 17054, 17057, 21653, 26683, 26686, 30588, };

const int8_t kIdx0_9[11] = {16, 1, -2, 1, 0, -1, -3, 0, 2, 3, -3, };
const int16_t kNlsf0_9[10] = {2619, 2622, 6933, 7754, 8983, 11626, 20482, 24491, 25399, 25402, };

const int8_t kIdx1_0[17] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, };
const int16_t kNlsf1_0[16] = {896, 2944, 4864, 6912, 8832, 10880, 12800, 14848, 16768, 18816, 20736, 22784, 24704, 26624, 28544, 30592, };

const int8_t kIdx1_1[17] = {20, 4, -4, 4, -4, 4, -4, 4, -4, 4, -4, 4, -4, 4, -4, 4, -4, };
const int16_t kNlsf1_1[16] = {2832, 2835, 7115, 7118, 10840, 10843, 14283, 14297, 17398, 17408, 20903, 20906, 24559, 24568, 27807, 27810, };

const int8_t kIdx1_2[17] = {2, -2, 0, 3, -1, 0, -4, -4, 2, -2, 2, 0, 4, -3, 2, -1, -3, };
const int16_t kNlsf1_2[16] = {1202, 3406, 4889, 4892, 4895, 5338, 9064, 15202, 16029, 19402, 20553, 21985, 21993, 25907, 26107, 28222, };

const int8_t kIdx1_3[17] = {17, -2, -3, 4, 4, -3, -3, 2, -3, 2, 2, 3, 4, -3, -4, 1, 2, };
const int16_t kNlsf1_3[16] = {213, 1869, 6917, 6920, 6923, 8757, 12604, 12618, 20198, 20732, 21242, 21245, 21253, 23557, 30355, 32067, };

const int8_t kIdx1_4[17] = {22, 4, 3, -4, 4, 3, -4, 2, -4, -2, -4, -3, 3, 2, 3, 1, -2, };
const int16_t kNlsf1_4[16] = {3077, 3082, 3122, 8040, 8043, 8046, 9750, 9764, 12770, 14763, 20414, 26159, 27021, 28148, 28155, 28488, };

const int8_t kIdx1_5[17] = {30, -4, 2, 1, 3, 3, -4, 0, 4, 4, 3, 0, 0, 4, 1, 1, -2, };
const int16_t kNlsf1_5[16] = {100, 7257, 9409, 11771, 11774, 11777, 18224, 21900, 22308, 22318, 22329, 24615, 27873, 27882, 28280, 29064, };

const int8_t kIdx1_6[17] = {22, -1, 1, 4, 0, 3, -2, 2, -4, 0, 1, 0, -1, -3, -2, -4, -3, };
const int16_t kNlsf1_6[16] = {2335, 4324, 6715, 6718, 8664, 8667, 10447, 10461, 15117, 16315, 16506, 17703, 17834, 20358, 23081, 27580, };

const int8_t kIdx1_7[17] = {1, 1, -1, -1, -4, 2, 3, 3, 4, 1, -4, 2, 1, 0, -2, 4, -2, };
const int16_t kNlsf1_7[16] = {1694, 2176, 4736, 7436, 14091, 16029, 16571, 16585, 16599, 16609, 22251, 22648, 23936, 25989, 29431, 29434, };

const int8_t kIdx1_8[17] = {9, -2, 2, 2, -2, -4, -1, 3, 1, 0, -2, 1, 1, 1, 1, -1, -4, };
const int16_t kNlsf1_8[16] = {726, 4613, 4653, 4656, 6862, 12478, 15978, 15992, 16722, 18393, 22314, 23616, 24820, 25681, 26147, 27531, };

const int8_t kIdx1_9[17] = {16, -2, 3, 0, 2, 2, 4, 2, 4, -3, -2, 2, 0, 2, 4, 1, 3, };
const int16_t kNlsf1_9[16] = {2176, 4007, 5527, 9212, 10038, 14000, 14005, 14411, 14425, 17218, 23134, 25101, 28924, 30911, 30918, 32421, };

struct NlsfCase {
    const int8_t* indices;
    const int16_t* nlsf;
    const player::audio::silk::NlsfCodebook* codebook;
    int order;
};

const NlsfCase kCases[] = {
    {kIdx0_0, kNlsf0_0, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx0_1, kNlsf0_1, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx0_2, kNlsf0_2, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx0_3, kNlsf0_3, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx0_4, kNlsf0_4, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx0_5, kNlsf0_5, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx0_6, kNlsf0_6, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx0_7, kNlsf0_7, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx0_8, kNlsf0_8, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx0_9, kNlsf0_9, &player::audio::silk::kNlsfCodebookNbMb, 10},
    {kIdx1_0, kNlsf1_0, &player::audio::silk::kNlsfCodebookWb, 16},
    {kIdx1_1, kNlsf1_1, &player::audio::silk::kNlsfCodebookWb, 16},
    {kIdx1_2, kNlsf1_2, &player::audio::silk::kNlsfCodebookWb, 16},
    {kIdx1_3, kNlsf1_3, &player::audio::silk::kNlsfCodebookWb, 16},
    {kIdx1_4, kNlsf1_4, &player::audio::silk::kNlsfCodebookWb, 16},
    {kIdx1_5, kNlsf1_5, &player::audio::silk::kNlsfCodebookWb, 16},
    {kIdx1_6, kNlsf1_6, &player::audio::silk::kNlsfCodebookWb, 16},
    {kIdx1_7, kNlsf1_7, &player::audio::silk::kNlsfCodebookWb, 16},
    {kIdx1_8, kNlsf1_8, &player::audio::silk::kNlsfCodebookWb, 16},
    {kIdx1_9, kNlsf1_9, &player::audio::silk::kNlsfCodebookWb, 16},
};

}  // namespace

TEST(SilkNlsf, MatchesReference) {
    for (const NlsfCase& test : kCases) {
        int16_t nlsf[16] = {};
        player::audio::silk::nlsfDecode(nlsf, test.indices, *test.codebook);
        for (int i = 0; i < test.order; ++i) {
            EXPECT_EQ(nlsf[i], test.nlsf[i]) << "coefficient " << i;
        }
    }
}

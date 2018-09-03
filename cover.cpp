#include <iostream>
#include <cstdio>
#include <cmath>

int
main()
{
    constexpr int K    = 8;
    constexpr int Bmax = 256;

    /* For all the possible batches... */
    for (int b = 1; b <= Bmax; b++) {
        /* For all the possible offsets in a cache line... */
        for (int ofs = 0; ofs < K; ofs++) {
            int lines = 0;

            /* For all the possible cache lilnes... */
            for (int k = 0; k <= Bmax; k += K) {
                /* Check if any slot in the cache line is covered by the
                 * batch. */
                for (int s = k; s < k + K; s++) {
                    if (s >= ofs && s < ofs + b) {
                        lines++;
                        break;
                    }
                }
            }

            double predict =
                static_cast<double>(b - 1) / static_cast<double>(K);
            predict = std::ceil(predict) + 1.0;

            int diff = static_cast<int>(predict) - lines;

            std::printf("B=%03d o=%03d lines=%03d (+%d)\n", b, ofs, lines,
                        diff);
            if (diff < 0 || diff > 1) {
                std::printf("Wrong formula\n");
                return -1;
            }
        }
    }

    return 0;
}

#include <iostream>
#include <cstdio>

int
main()
{
    constexpr int K    = 8;
    constexpr int Bmax = 256;

    for (int b = 1; b < Bmax; b++) {
        for (int ofs = 0; ofs < K; ofs++) {
            int lines = 0;

            for (int k = 0; k <= Bmax; k += K) {
                for (int s = k; s < k + K; s++) {
                    if (s >= ofs && s < ofs + b) {
                        lines++;
                        break;
                    }
                }
            }

            std::printf("B=%03d o=%03d lines=%03d\n", b, ofs, lines);
        }
    }

    return 0;
}

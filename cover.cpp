#include <iostream>
#include <cstdio>
#include <cmath>
#include <unistd.h>

int
covered_cachelines(int first, int b, int L, int K)
{
    int fk    = (first / K) * K;
    int lines = 0;

    /* For all the possible cache lines... */
    for (int k = fk; k <= L; k += K) {
        /* Check if any slot in the cache line is covered by the
         * batch. */
        for (int s = k; s < k + K; s++) {
            if (s >= first && s < first + b) {
                lines++;
                break;
            }
        }
    }

    return lines;
}

int
cover()
{
    constexpr int Bmax = 256;
    constexpr int K    = 8;
    int total_error    = 0;

    /* For all the possible batches... */
    for (int b = 1; b <= Bmax; b++) {
        /* For all the possible offsets in a cache line... */
        for (int ofs = 0; ofs < K; ofs++) {
            int lines = covered_cachelines(ofs, b, Bmax, K);

            double predict =
                static_cast<double>(b - 1) / static_cast<double>(K);
            predict = std::ceil(predict) + 1.0;

            int diff = static_cast<int>(predict) - lines;

            total_error += std::abs(diff);

            std::printf("B=%03d o=%03d lines=%03d (+%d)\n", b, ofs, lines,
                        diff);
            if (diff < 0 || diff > 1) {
                std::printf("Wrong formula\n");
                return -1;
            }
        }
    }

    std::printf("Total error = %d\n", total_error);

    return 0;
}

int
roll()
{
    constexpr int L    = 4096;
    constexpr int Bmax = 64;
    constexpr int K    = 8;
    double total_error = 0;
    int cases          = 0;

    /* For all the possible batches... */
    for (int b = 1; b <= Bmax; b++) {
        for (int ofs = 0; ofs < b; ofs++) {
            int misses = 0;

            for (int cur = ofs; cur < L; cur += b) {
                misses += covered_cachelines(cur, b, L, K);
            }

            double rate = static_cast<double>(misses) / static_cast<double>(L);
#if 1
            double predict =
                static_cast<double>(b - 1) / static_cast<double>(K);
            predict = std::ceil(predict) + 1.0;
            predict /= b;
#else
            double binv = 1.0/static_cast<double>(b);
            double kinv = 1.0/static_cast<double>(K);
            double predict = binv;

            if (binv > kinv) {
                predict += binv;
            } else {
                predict += kinv;
            }
#endif

            double diff = predict - rate;
            total_error += std::abs(diff);
            cases++;

            std::printf("B=%03d ofs=%03d rate=%.2f predict=%.2f diff=%.4f\n", b,
                        ofs, rate, predict, diff);
            if (diff < -0.01) {
                std::printf("Wrong upper bound\n");
                return -1;
            }
        }
    }

    std::printf("Total error = %.2f\n", total_error);

    return 0;
}

static void
usage(const char *progname)
{
    std::printf("%s: [-hcr]\n", progname);
}

int
main(int argc, char **argv)
{
    int opt;

    while ((opt = getopt(argc, argv, "hcr")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;
            break;

        case 'c':
            return cover();
            break;

        case 'r':
            return roll();
            break;

        default:
            usage(argv[0]);
            return 0;
            break;
        }
    }

    return 0;
}

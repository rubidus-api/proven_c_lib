#include "example.h"
#include <string.h>

/*
 * 다른 예제는 어느 생성기를 고를지를 보인다. 이것은 그 밑의 층에 대한 것이다. 난수가
 * *어디서* 오는가, 그리고 그것을 신경 쓰지 않는 코드를 어떻게 쓰는가.
 *
 *   proven_rng_t 는 난수 바이트의 원천을 포인터 한 쌍으로 나타낸 것이다 - 작은 함수
 *   표와 그 함수들이 다룰 생성기 상태. proven_rng_t 를 받는 코드는 OS 생성기에서도,
 *   ChaCha20 에서도, xoshiro 에서도, 시험용으로 여러분이 지어낸 가짜에서도 한 줄도
 *   고치지 않고 돈다.
 *
 *   proven_random_set_source 는 *그보다* 아래 층이다. 생성기가 씨를 받는 날 엔트로피가
 *   어디서 오는가. 호스트가 있는 프로그램에는 이미 하나가 있고 - 운영체제의 것 - 그대로
 *   두어야 한다. 베어메탈 프로그램에는 없고, 그 기계의 하드웨어 원천을 다는 자리가
 *   이 고리다.
 *
 * 고정된 씨앗 부분은 보기보다 중요하다. *알려진* 씨앗으로 씨를 뿌린 암호용 생성기는
 * 알려진 수열을 내놓고, 그것이 난수가 끼는 시험을 "일주일에 한 번 실패" 가 아니라
 * 재현 가능한 것으로 만든다.
 */

/* 전혀 무작위가 아닌 "엔트로피" 원천이다. 그냥 센다. 이런 것이 실제 프로그램에 들어갈
 * 자리는 없고 - 장의 반례를 볼 것 - 다만 이 고리가 어떻게 도는지 보이기에, 그리고 돌
 * 때마다 같은 바이트를 내야 하는 시험에 딱 맞는 모양이다. */
static bool counting_entropy(void *ctx, void *buf, proven_size_t len) {
    proven_u8 *next = (proven_u8 *)ctx;
    proven_u8 *out = (proven_u8 *)buf;
    for (proven_size_t i = 0; i < len; ++i) {
        out[i] = (*next)++;
    }
    return true;
}

/* 특성에 기대어 쓴 함수. 자기가 어느 생성기를 받았는지 끝내 알지 못한다. */
static proven_u64 roll_total(proven_rng_t rng, int rolls) {
    proven_u64 sum = 0;
    for (int i = 0; i < rolls; ++i) {
        sum += proven_rng_below(rng, 6) + 1;
    }
    return sum;
}

int main(void) {
    /* --- 1. 쓰기 전에 확인할 수 있는 원천 --------------------------------- */

    proven_rng_t nothing = {0};
    EXAMPLE_REQUIRE(!proven_rng_is_valid(nothing), "a zero-initialised source is not a generator");

    /* 잘못된 원천에서 뽑아도 죽지 않고 수를 지어내지도 않는다. 0 을 돌려준다. 정의된,
     * 심심한 답이다 - 그런데 0 의 연속은 난수가 아니므로, 뽑을 때마다 믿는 대신 받을 때
     * 한 번 원천을 확인할 것. */
    EXAMPLE_REQUIRE(proven_rng_u64(nothing) == 0, "an invalid source yields 0, not a fabricated value");

    /* --- 2. *알려진* 씨앗에서 나온 암호용 생성기 --------------------------- */

    /* proven_chacha_rng_seed 는 씨앗 바이트를 곧장 받으므로 수열이 재현된다. 시험에서는
     * 그것이 원하는 바이고 운영에서는 결코 아니다. 씨앗을 알아낸 사람은 그 생성기가
     * 내놓을 모든 바이트를 안다. */
    proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE];
    memset(seed, 0xA5, sizeof seed);

    proven_chacha_rng_t a, b;
    proven_chacha_rng_seed(&a, seed);
    proven_chacha_rng_seed(&b, seed);

    /* next 는 한 번에 64비트 낱말 하나를 돌려준다. 같은 씨앗을 받은 생성기 둘은 같은
     * 수열을 걷는다 - 시험이 기대는 성질이 그것이다. */
    proven_u64 first = proven_chacha_rng_next(&a);
    EXAMPLE_REQUIRE(first == proven_chacha_rng_next(&b), "the same seed replays the same sequence");
    EXAMPLE_REQUIRE(proven_chacha_rng_next(&a) == proven_chacha_rng_next(&b), "and keeps replaying it");

    /* --- 3. 특성을 통해 쓰기 ---------------------------------------------- */

    proven_rng_t rng = proven_chacha_rng(&a);
    EXAMPLE_REQUIRE(proven_rng_is_valid(rng), "a seeded generator makes a valid source");

    proven_u64 word = proven_rng_u64(rng);
    (void)word;   /* 어떤 64비트 값이든 옳은 답이다. 단언할 것이 없다 */

    /* fill 은 무더기로 하는 꼴이다. 나머지를 스스로 처리해야 하는 64비트 낱말 반복문
     * 대신, 버퍼 하나를 한 번의 호출로 채운다. */
    proven_byte_t nonce[12] = {0};
    proven_rng_fill(rng, nonce, sizeof nonce);

    bool all_zero = true;
    for (proven_size_t i = 0; i < sizeof nonce; ++i) {
        if (nonce[i] != 0) all_zero = false;
    }
    EXAMPLE_REQUIRE(!all_zero, "filling from a seeded generator must produce something");

    /* 같은 함수를, 서로 다른 생성기 둘이 굴린다. 이 특성이 존재하는 이유는 이것 하나다. */
    proven_chacha_rng_t c;
    proven_chacha_rng_seed(&c, seed);
    proven_u64 crypto_total = roll_total(proven_chacha_rng(&c), 50);

    proven_xoshiro256ss_t fast;
    proven_xoshiro256ss_seed(&fast, 7);
    proven_u64 fast_total = roll_total(proven_xoshiro256ss_rng(&fast), 50);

    EXAMPLE_REQUIRE(crypto_total >= 50 && crypto_total <= 300, "50 dice must total between 50 and 300");
    EXAMPLE_REQUIRE(fast_total >= 50 && fast_total <= 300, "whichever generator produced them");

    /* --- 4. 생성기를 쥐지 않고 강한 낱말 하나 ----------------------------- */

    /* proven_random_u64 는 엔트로피 원천에서 곧장 뽑는다. 한 번뿐인 일에는 편하고 -
     * 시작할 때 잡는 표의 해시 키, 요청 번호 - 반복문에는 틀린 도구다. 호출마다 운영체제로
     * 다녀오는 값이 들기 때문이다. 무더기로 뽑을 때는 생성기에 한 번 씨를 뿌리고 거기서
     * 뽑을 것. */
    proven_u64 one_off = proven_random_u64();
    proven_u64 another = proven_random_u64();
    EXAMPLE_REQUIRE(one_off != another || one_off != 0,
                    "two draws from the OS source are essentially never the same value");

    /* --- 5. 엔트로피 원천 달기 -------------------------------------------- */

    /* 호스트가 있는 대상에서는 운영체제의 원천이 이미 달려 있고 그대로 두어야 한다. 이
     * 고리는 베어메탈을 위한 것이다. 거기서는 그 보드의 엔트로피가 어느 하드웨어
     * 레지스터에 사는지 라이브러리가 알 길이 없다. 여기서는 일부러 가짜 원천을 달았다.
     * 오직 그 장치를 보이고 갈아 끼우기가 먹혔음을 증명하기 위해서다 - 진짜는 진짜
     * 하드웨어 엔트로피여야 한다. */
    proven_u8 counter = 0;
    proven_random_set_source(counting_entropy, &counter);

    proven_byte_t drawn[4] = {0};
    EXAMPLE_REQUIRE(proven_random_bytes(drawn, sizeof drawn), "the installed source must answer");
    EXAMPLE_REQUIRE(drawn[0] == 0 && drawn[1] == 1 && drawn[2] == 2 && drawn[3] == 3,
                    "and it is the source we installed that answered");

    /* 플랫폼 기본값을 되돌려 놓는다. 시험용 원천을 그대로 둔 채로 두는 것이, 프로그램이
     * 운영에서 예측 가능한 키를 만들어 내게 되는 방법이다. */
    proven_random_set_source(NULL, NULL);

    proven_byte_t real[8] = {0};
    EXAMPLE_REQUIRE(proven_random_bytes(real, sizeof real), "the OS source is back and working");

    printf("random: first word %llu, dice totals %llu and %llu\n",
           (unsigned long long)first, (unsigned long long)crypto_total, (unsigned long long)fast_total);

    return EXAMPLE_OK();
}

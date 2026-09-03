#include "example.h"

/*
 * 쓰임새로 나눈 난수. "난수" 하나는 없다. 똑같이 생겼지만 같지 않은 할 일이 둘 있고,
 * 잘못 고르는 것이 위험의 전부다.
 *
 *   키, 토큰, 논스 - 공격자가 알아맞히면 안 되는 것 - 에는 *암호용* 난수원이 필요하다.
 *   시뮬레이션, 시험, 게임에는 *재현 가능한* 것이 필요하다. 다시 돌려 볼 수 없는 실패한
 *   실행은 디버깅할 수 없는 실패한 실행이기 때문이다. 두 요구는 정면으로 부딪친다.
 *   재현 가능하다는 것은 예측 가능하다는 뜻이고, 예측 가능한 것이야말로 토큰이 되어서는
 *   안 되는 것이다. 그래서 라이브러리는 둘에 다른 이름을 주고, 선택은 씨앗을 어떻게
 *   뿌렸는지에 묻히는 대신 여기 호출 자리에서 눈에 보인다.
 */

int main(void) {
    /* ---- 할 일 1: 비밀. OS 의 CSPRNG - 그리고 난수가 실패할 수 있는 유일한 자리. ---- */
    proven_byte_t key[32];
    EXAMPLE_REQUIRE(proven_random_bytes(key, sizeof key),
                    "the OS must give us strong bytes on a hosted platform");

    /* ---- 할 일 2: 암호용 바이트를 아주 많이, 또는 OS 가 없는 보드에서 조금이라도.
     * ChaCha20 은 순수한 산술이다. 진짜 엔트로피로 한 번 씨를 뿌리면 그 뒤로는 운영체제가
     * 필요 없다 - 뽑을 때마다 시스템 호출을 하지 않고, 베어메탈에서도 돈다.
     * 씨 뿌리기가 실패할 수 있는 *유일한* 걸음이므로, 확인해야 할 것도 그것 하나다. ---- */
    proven_chacha_rng_t crypto;
    EXAMPLE_REQUIRE(proven_chacha_rng_seed_from_entropy(&crypto), "seed the CSPRNG from the OS, once");

    proven_byte_t token[16];
    proven_chacha_rng_fill(&crypto, token, sizeof token);   /* 실패할 수 없다: 이미 씨가 뿌려져 있다 */

    /* ---- 할 일 3: *재현되는* 실행. xoshiro256** 은 빠르고 씨앗에서 똑같이 다시
     * 재생된다. 그것이 실패한 시뮬레이션을 디버깅할 수 있게 만든다. 비밀급이 *아니다* -
     * 출력 몇 개면 상태 전체가 드러난다. 토큰을 만들라고 건네는 일은 절대 없어야 한다. ---- */
    proven_xoshiro256ss_t sim;
    proven_xoshiro256ss_seed(&sim, 12345);

    proven_xoshiro256ss_t replay;
    proven_xoshiro256ss_seed(&replay, 12345);
    EXAMPLE_REQUIRE(proven_xoshiro256ss_next(&sim) == proven_xoshiro256ss_next(&replay),
                    "the same seed replays the same run - that is the whole point");

    /* ---- 도우미들은 proven_rng_t 특성을 통해 *어떤* 난수원 위에서도 돈다. ---- */
    proven_rng_t rng = proven_xoshiro256ss_rng(&sim);

    /* 범위 안의 수. `rng_u64() % 6` 은 모두가 쓰는 것이고, 상한이 2^64 를 나누어떨어지게
     * 하지 않는 한 *치우쳐* 있다 - 작은 값이 더 자주 나온다. 이것은 그렇지 않다. */
    for (int i = 0; i < 100; ++i) {
        proven_u64 die = proven_rng_below(rng, 6) + 1;
        EXAMPLE_REQUIRE(die >= 1 && die <= 6, "a die roll is 1..6, uniformly");
    }

    proven_i64 temperature = proven_rng_range(rng, -40, 85);
    EXAMPLE_REQUIRE(temperature >= -40 && temperature <= 85, "an inclusive range, both ends");

    double p = proven_rng_f64(rng);
    EXAMPLE_REQUIRE(p >= 0.0 && p < 1.0, "a double in [0, 1) - never 1.0");

    /* 치우치지 않은 섞기: 위의 치우치지 않은 인덱스 위에서 도는 피셔-예이츠. 이 반복문의
     * `% n` 판은 어떤 순서를 눈에 띄게 더 좋아한다. */
    int deck[10];
    for (int i = 0; i < 10; ++i) deck[i] = i;
    proven_rng_shuffle(rng, deck, 10, sizeof deck[0]);

    int sum = 0;
    for (int i = 0; i < 10; ++i) sum += deck[i];
    EXAMPLE_REQUIRE(sum == 45, "a shuffle is a permutation: every card is still there, once");

    /* 암호용 생성기도 같은 특성을 만족하므로, 고르기만 하면 되는 것이 아니라 알아맞힐 수
     * 없어야 할 때 같은 도우미를 그대로 쓸 수 있다. */
    proven_rng_t secure = proven_chacha_rng(&crypto);
    proven_u64 unguessable_index = proven_rng_below(secure, 1000);
    EXAMPLE_REQUIRE(unguessable_index < 1000, "the helpers do not care which source they draw from");

    (void)token;
    (void)key;
    return EXAMPLE_OK();
}

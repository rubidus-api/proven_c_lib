#include "example.h"

/*
 * 1과 - 한 줄을 빌드해서 돌려 본다.
 *
 * 아직 이 라이브러리의 생각에 대한 것은 하나도 없다. 이 프로그램이 답하는 물음은
 * "내 빌드 명령이 되는가" 하나뿐이고, 그것만 따로 물어 볼 값어치가 있다. 뒤의
 * 모든 과가 그것을 전제하기 때문이다.
 *
 * printf(fmt, ...) 는 검사되지 않는다. "%d" 자리에 double 을 건넸다고 컴파일러가
 * 알려 주지 못한다. proven_println 은 검사한다. 인자마다 PROVEN_ARG 로 감싸
 * 자기 타입을 함께 들고 가기 때문이다.
 */

int main(void) {
    proven_println("hello from proven");

    /* {} 가 자리표다. 값은 PROVEN_ARG 를 거치면서 어떤 타입인지를 함께 싣는다.
     * 그래서 서식 문자열과 인자가 어긋날 수가 없다. */
    proven_println("one number: {}", PROVEN_ARG(42));
    proven_println("two of them: {} and {}", PROVEN_ARG(1), PROVEN_ARG(2));

    return EXAMPLE_OK();
}

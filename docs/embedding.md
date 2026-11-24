# 호스트 임베딩

게임 엔진처럼 자기 Lua VM을 가진 호스트에 Arky 런타임을 넣는 방법이다. 런타임은 async/await의 스케줄러, [worker Task](tasks.md), [파일·TCP I/O](io.md), 정적 import를 한 Lua 상태에 설치한다. CLI(`arky run`)를 쓰지 않고 생성된 Lua만 실행하는 호스트를 위한 것이다.

## 빌드

호스트는 `runtime/arky_runtime.cpp` 한 파일을 C++20으로 컴파일하고 자기 LuaJIT와 링크한다. include 경로에 이 저장소의 루트와 LuaJIT 헤더를 둔다. Windows는 `ws2_32`를 함께 링크한다.

Task·I/O 모듈과 스케줄러가 이 한 파일에 들어 있다. `tynex_task`·`tynex_io` 공유 모듈이나 `runtime/*.lua`를 따로 배포하지 않는다.

## 사용

```cpp
#include "runtime/arky_runtime.h"

arky::runtime::options settings;
settings.import_loader = [](std::string_view name, arky::runtime::module_source& module) {
    return read_generated_lua(name, module.chunk, module.name);   // 호스트의 파일 시스템
};
arky::runtime::open(L, settings);

arky::runtime::import_module(L, "game.main");   // 진입 모듈을 실행하고 그 값을 스택에 올린다
lua_pop(L, 1);

// 매 프레임, Lua 상태를 소유한 스레드에서. 프레임 시간(초)을 넘긴다
arky::runtime::step(L, delta_seconds);
```

| 함수 | 역할 |
| --- | --- |
| `open(L, options)` | 런타임을 설치한다. 한 Lua 상태에 하나이며 두 번 열면 오류다 |
| `is_open(L)` | `open`과 `close` 사이에 참 |
| `step(L, max_resumes = 64)` | 기한이 된 타이머를 완료하고, 끝난 네이티브 작업을 거두고, 준비된 async 함수를 최대 `max_resumes`번 재개한다. 기다리며 멈추지 않는다. 남은 Task가 있으면 참 |
| `step(L, delta_seconds, max_resumes = 64)` | 같은 일을 하되 호스트 시간을 `delta_seconds`만큼 진행시킨다 |
| `import_module(L, name)` | `import`와 같은 규칙으로 모듈을 로드해 값을 스택에 올린다. 모듈이 호출한 async 함수는 이 안에서 첫 `await`까지 실행된다 |
| `close(L)` | worker 풀을 멈추고 런타임을 없앤다. 생략해도 `lua_close`가 모두 해제한다 |
| `deferred` | 호스트가 나중에 완료시키는 Task의 핸들 |

실패는 `arky::error` 예외로 알린다. 호출자의 프레임을 지나가는 Lua 오류는 내지 않는다.

## Lua 상태마다 하나

런타임의 상태(스케줄러 큐, import 캐시, I/O 소유자, worker 풀)는 모두 그 Lua 상태의 레지스트리에 있고 상태와 함께 해제된다. 프로세스 전역 상태는 없다. 따라서 도구·테스트용 VM이나 다시 시작한 게임 VM이 각자 런타임을 가질 수 있고, 서로 영향을 주지 않는다. 각 런타임은 자기 Lua 상태를 소유한 스레드에서만 쓴다.

스크립트를 다시 시작할 때는 `close` 후 같은 상태에서 다시 `open`하거나 새 상태에서 `open`한다. 새 런타임의 import 캐시는 비어 있다.

## 생성 코드가 보는 이름

`scheduler`, `fs`, `net`과 컴파일러가 쓰는 `__tynex_import`, `__tynex_async_runtime`은 전역 변수로 만들지 않는다. Arky 모듈의 환경에만 있으므로 레거시 스크립트의 전역과 겹치지 않는다. 그 환경은 다른 이름을 `_G`에서 읽고 `_G`에 쓴다.

런타임은 이 환경을 `package.loaded.tynex_runtime.environment`로 내놓는다. 이 이름들을 쓰는 Arky 모듈(async, 정적 import, `scheduler`·`fs`·`net`)은 컴파일러가 첫 줄에 넣은 코드로, 환경 없이 로드되면 스스로 이 환경으로 바꾼다. 그래서 레거시 코드가 호스트의 평소 로더로 `require("game.panel")`처럼 불러도 동작한다. 원문이든 바이트코드든, 암호화된 팩에 있든 상관없다. 필요한 이름을 호스트가 직접 넣어 둔 경우(예: 전역 `__tynex_import`)에는 바꾸지 않는다. 런타임이 없으면 환경을 바꾸지 않고, 모듈이 `async requires the Tynex async runtime` 같은 오류로 바로 멈춘다. 이 이름들을 쓰지 않는 Arky 모듈은 보통의 Lua와 같아서 어디서나 그대로 동작한다.

`require("tynex_task")`와 `require("tynex_io")`는 런타임에 들어 있는 모듈을 돌려준다. [Task 예제](../examples/tasks/README.md)의 레거시 래퍼 모듈(`return require("tynex_task")`)이 그대로 동작한다. 호스트가 같은 이름으로 먼저 로드한 모듈이 있으면 바꾸지 않는다.

## import 로더

로더를 주지 않으면 `import x from "name"`은 Lua의 `require("name")`이다. 게임 엔진처럼 모듈 로더를 이미 가진 호스트에 맞는 방식이다. 정적 import와 레거시 `require`가 `package.loaded`를 함께 써서 같은 모듈을 두 번 실행하지 않고, 호스트의 로더(검색 경로, 바이트코드, 복호화)를 그대로 쓴다. `import_module`도 같다.

로더를 주면 `import x from "name"`은 `options::import_loader`를 부른다. 로더는 모듈의 Lua를 `module.chunk`에 담고 참을 돌려주며, 그런 모듈이 없으면 거짓을 돌려준다. `chunk`는 생성된 Lua 원문이나 LuaJIT 바이트코드다. `module.name`은 오류 메시지와 traceback에 쓰이며 비워 두면 모듈 이름을 쓴다.

- 로더로 읽은 모듈은 Arky 모듈의 환경에서 한 번 실행하고 값을 캐시한다. 값을 돌려주지 않는 모듈의 값은 `true`다. 이 캐시는 `package.loaded`와 따로다.
- 순환 import와 로더가 모르는 모듈은 오류다(`cyclic static import`, `unprepared static import`).
- 실패한 모듈은 캐시하지 않으며, 모듈이 낸 오류 값은 바꾸지 않고 import한 코드에 전달한다.

`arky gen FILE`은 그 파일의 Lua만 출력한다. 배포할 때는 모듈마다 실행해 로더가 읽을 파일을 만든다. 프로젝트 전체를 한 번에 내보내는 명령은 아직 없다. 레거시 `.lua` 모듈은 원본을 그대로 넘긴다.

CLI와 달리 검사된 모듈 목록이 없으므로, 로더가 돌려준 내용이 검사를 거친 것인지는 호스트의 빌드 과정이 보장해야 한다. 소스 맵으로 원본 행을 되찾는 처리도 하지 않는다.

## 프레임 시간과 타이머

`scheduler.sleep(milliseconds)`는 호스트 시간으로 기다린다. 호스트 시간은 `step(L, delta_seconds)`에 넘긴 만큼만 흐르므로, 게임이 일시정지 중에 0을 넘기거나 배속을 곱해 넘기면 타이머에 그대로 적용된다. `delta_seconds` 없이 `step(L)`만 부르면 호스트 시간이 실제 시간을 따른다. `scheduler.sleep_realtime(milliseconds)`는 어느 쪽이든 실제 시간으로 기다린다.

`sleep`은 타이머를 만든 프레임의 시작부터 센다. 프레임 사이에 만든 타이머(예: 호스트가 `import_module`로 모듈을 처음 실행할 때)는 다음 step의 시각부터 센다. 다음 step의 `delta_seconds`는 그 타이머를 만들기 전에 지난 시간을 담고 있기 때문이다.

단위가 다르다는 점에 주의한다. 호스트는 엔진의 프레임 시간 그대로 **초**를 넘기고(`delta_seconds`), 스크립트의 타이머는 **밀리초**를 받는다. 0 이상인 유한한 수가 아닌 `delta_seconds`는 오류다.

`step(L, 1)`처럼 정수를 넘기면 `max_resumes`로 해석된다. 프레임 시간은 `float`나 `double`로 넘긴다.

## 호스트가 완료시키는 Task

응답이 오거나 애니메이션이 끝났을 때 완료되는 Task는 `arky::runtime::deferred`로 만든다. 바인딩 함수가 Task를 스크립트에 돌려주고 핸들을 보관했다가 나중에 완료시킨다. 스크립트의 [`scheduler.deferred()`](async-await.md#밖에서-완료시키는-task)와 같은 Task다.

```cpp
int http_get(lua_State* L) {
    arky::runtime::deferred done(L);                 // Task를 L의 스택에 올린다
    request(url, [done](const Response& response) {  // 나중에, Lua 상태를 소유한 스레드에서
        if (response.ok) done.resolve(response.body, response.status);
        else done.reject("request failed");
    });
    return 1;                                        // 스크립트는 Task를 받아 await한다
}
```

- `resolve(values...)`는 바인딩 함수의 결과와 같은 규칙으로 값을 변환해 Task를 완료한다. `reject(message)`는 그 메시지를 오류로 실패시킨다.
- 둘 다 그 호출이 Task를 완료했을 때만 참이다. 이미 완료·실패했거나 스크립트가 취소한 Task에는 아무 일도 하지 않는다.
- 핸들을 복사하면 같은 Task를 가리킨다. 핸들은 런타임도 Lua 상태도 붙잡지 않는다. 런타임을 `close`했거나 Lua 상태가 닫힌 뒤에는 `alive()`가 거짓이고 `resolve`·`reject`는 거짓을 돌려주고 끝난다. 늦게 도착한 콜백이 닫힌 런타임이나 VM을 건드리지 않으며, 같은 Lua 상태에서 런타임을 다시 열어도 이전 핸들은 살아나지 않는다.
- 핸들은 async 함수 안에서 불린 바인딩 함수에서 만들어도 된다. 완료는 런타임이 가진 별도 coroutine에서 실행하므로, 그 async 함수가 멈춰 있어도 문제없다.
- 답을 이미 알고 있으면(캐시에 있는 응답, 잘못된 인자) 핸들을 만든 바인딩 함수 안에서 곧바로 완료해도 된다. 스크립트는 완료된 Task를 받는다.
- 핸들의 사용과 소멸은 Lua 상태를 소유한 스레드에서 한다. 다른 스레드에서 끝난 작업은 그 스레드로 넘긴 뒤 완료시킨다.

## 파일 읽기

`options::file_reader`를 주면 `fs.read_file`이 운영체제의 파일 대신 호스트가 준 바이트를 받는다. 파일을 아카이브에 넣어 두는 호스트를 위한 것이다.

- 리더는 **런타임의 worker 스레드**에서 실행되고 여러 읽기가 동시에 실행될 수 있다. Lua 상태를 건드리지 않아야 하고 스레드에 안전해야 한다.
- `bytes`에 `max_bytes` 이하를 담고 참을 돌려준다. 더 많이 담으면 그 읽기는 `file exceeds max_bytes`로 실패한다.
- 거짓을 돌려주면 `error`의 메시지로 Task가 실패한다.
- 리더가 있으면 모든 `fs.read_file`이 리더로 간다.

## worker Task의 JIT

worker Task는 Task마다 새 Lua 상태에서 실행된다. 기본으로는 그 상태에서 LuaJIT의 JIT가 켜져 있다. 자기 Lua 상태를 인터프리터로 돌리는 호스트(예: 서버 검증과 같은 실행 방식을 맞추는 게임)는 `options::task_jit = false`로 열어 Task 상태의 JIT도 끈다. 호스트 자신의 Lua 상태 JIT는 바꾸지 않는다. 호스트가 `luaJIT_setmode`로 따로 끈다.

```cpp
arky::runtime::options settings;
settings.task_jit = false;          // Task를 실행하는 Lua 상태마다 luaJIT_setmode(..., LUAJIT_MODE_OFF)
arky::runtime::open(L, settings);
```

## 종료

`close`는 worker 풀을 멈춘다. 실행 중인 worker 작업은 다음 `checkpoint()`에서 취소되며, 그때까지 `close`가 기다린다. 아직 끝나지 않은 Task는 완료되지 않고, 그 런타임의 `deferred` 핸들은 그때부터 아무 일도 하지 않는다. `close` 없이 `lua_close`만 해도 같은 순서로 해제된다.

호스트가 직접 만든 Lua 상태(`luaL_newstate`·`lua_close`)도 같다. `lua_close`가 finalizer를 실행하는 동안 런타임이 먼저 정리될 수 있는데, 그 뒤에 실행된 finalizer가 새 `deferred`를 만들거나 import를 하면 `the Arky runtime is closing` 오류로 거절한다.

## 검증 범위

`tests/runtime/host.cpp`가 설치·재설치, import 규칙, 프레임 단위 진행, worker Task, 파일 리더, 타이머, async 함수의 깊은 재귀, 호스트가 완료시키는 Task, Lua 상태 사이의 분리, 작업이 남은 채 닫히는 Lua 상태, 호스트가 직접 닫는 Lua 상태를 확인한다(`make task-test`, ASan/UBSan은 `make binder-sanitize`).

| 대상 | 상태 |
| --- | --- |
| macOS | 빌드와 테스트 실행 |
| Linux | CI(`binder.yml`)에서 빌드와 테스트 실행 |
| Windows x64 (MSVC, clang-cl) | 빌드와 테스트 실행. CI(`windows.yml`)는 clang-cl |
| iOS arm64, Android (arm64-v8a, armeabi-v7a, x86, x86_64) | 컴파일만 확인. 기기 실행은 아직 확인하지 않았다 |

`runtime/async.lua`나 `runtime/host.lua`를 고치면 `make embed-runtime`으로 `runtime/arky_runtime_lua.inc`를 다시 만든다. 오래된 채로 두면 테스트가 실패한다.

## 아직 없는 것

- 디버거 연결과 소스 맵
- 여러 런타임이 worker 스레드 풀을 함께 쓰는 설정
- 실패했는데 아무도 `await`하지 않은 Task를 호스트에 알리는 콜백

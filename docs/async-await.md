# async / await

`.arky`의 `async function`, `await`, 결과 타입 `Task<T>`를 지원한다. async 함수를 호출하면 본문이 메인 Lua VM의 coroutine으로 그 자리에서 [첫 `await`까지 실행](#시작과-재개)되고 Task가 반환된다. **async 함수마다 OS 스레드를 만들지 않는다.** 실제 병렬 계산은 기존 [worker Task](tasks.md)로 보낸다. 공식 LuaJIT은 수정하지 않는다.

```lua
local async function double(n: number): number
   return n * 2
end
local async function main(): number
   local first = double(21)
   local second = double(15)
   return (await first) + (await second)
end
print(scheduler.run(main())) -- 72
```

CLI는 `scheduler.run()`으로 결과를 기다린다. 게임·UI에서는 호스트가 매 프레임 `scheduler.step()`을 호출한다. CLI가 남은 작업을 자동으로 끝까지 실행하지는 않는다.

## 문법과 타입

- `local async function f(...)`, `async function module:method(...)`, `async function(...)` 함수 값을 지원한다. class·trait 기본 메서드에도 `async function`을 쓴다.
- `async function f(): number`의 본문은 number를 반환하고 `f()`의 타입은 `Task<number>`다. 함수 값의 계약은 `async function(number): number`로 표기한다.
- `Task<number, string>`은 두 반환값, `Task<>`는 반환값 없음, `Task<number, ...string>`은 가변 반환이다. Task는 내장 타입이며 일반 사용자 제네릭 문법은 아직 없다.
- async 반환형의 고정 위치는 실제로 반환해야 한다. `: number?`는 `return nil`로 한 값을 반환하며 빈 return·암묵적인 fallthrough는 허용하지 않는다. 반환형을 생략하면 실제 반환 개수와 타입을 추론한다.
- await는 async 본문에서만 허용한다. 중첩된 일반 함수와 클래스 필드 기본값에서는 직접 사용할 수 없다. 기본값으로 async 함수 값을 만드는 것은 가능하다.
- await는 한 단계만 풀어준다. `Task<Task<number>>`를 한 번 기다리면 `Task<number>`다.
- Lua 다중 반환·가변 인수·끝의 nil을 유지한다. 마지막 표현식의 await는 다중 값으로 확장하고 `(await task)`는 한 값으로 줄어든다. `await task`만 문장으로 써서 결과를 버릴 수도 있다.
- `await f() + 1`은 `(await f()) + 1`이다. nullable Task는 nil을 먼저 확인해야 한다. 명시적 any 경계도 실행 시 managed Task인지 검사한다.
- await 뒤에는 다른 작업이 값을 변경했을 수 있으므로 캡처 변수의 좁힌 타입을 되돌린다. imported async 함수와 Task 결과도 같은 검사기로 확인한다.

## 스케줄러와 핸들

| API | 동작 |
| --- | --- |
| `scheduler.step(max_resumes?, elapsed?)` | 기한이 된 타이머를 완료하고 최대 지정 횟수만큼 coroutine 재개. 기본 64. `elapsed`는 지난 호스트 시간(밀리초). 남은 작업이 있으면 true |
| `scheduler.run(task)` | 해당 Task가 완료될 때까지 진행하고 결과 반환 |
| `scheduler.pending()` | 미완료 managed Task 수. worker·I/O 변환 핸들도 포함 |
| `scheduler.from_worker(handle)` | 네이티브 worker 핸들을 `Task<any>`로 연결 |
| `scheduler.sleep(milliseconds)` | 호스트 시간으로 그만큼 지나면 완료되는 `Task<>`. `sleep(0)`은 다음 step에서 완료 |
| `scheduler.sleep_realtime(milliseconds)` | 실제 시간으로 그만큼 지나면 완료되는 `Task<>` |
| `scheduler.deferred()` | 밖에서 완료시키는 Task. `task, resolve, reject` 세 값을 반환 |
| `scheduler.all(tasks)` | 배열의 Task가 모두 성공하면 완료되는 Task. 값은 각 Task의 첫 값의 배열 |
| `scheduler.race(tasks)` | 배열의 Task 중 가장 먼저 끝난 것과 같게 끝나는 Task |
| `task:ready()` | 성공·실패·취소가 확정되었으면 true |
| `task:status()` | queued(재개 대기) / running / waiting / succeeded / failed / cancelled |
| `task:result()` | 완료 결과 반환. 미완료·실패·취소는 오류 |
| `task:cancel()` | 최초 취소면 true. 이미 완료·취소되었으면 false |

step은 native 작업 완료를 기다리며 블로킹하지 않는다. 현재 구현은 native 핸들의 완료 상태를 확인하고 준비된 coroutine을 FIFO 큐에서 재개한다. run은 실행 가능한 coroutine이 없고 native 작업만 남으면 짧게 sleep하며 확인한다. OS 알림 기반 I/O 이벤트 루프는 아니다.

**재개 횟수는 프레임 실행 시간의 상한이 아니다.** async 본문의 긴 계산이나 블로킹 함수는 메인 스레드를 멈춘다. 완료된 Task의 await는 기다리지 않고 바로 값을 돌려준다. CPU 작업은 worker 풀에 맡긴다. run·step을 async 안에서 다시 호출하는 것은 정적 검사와 런타임에서 거절한다.

같은 VM의 async 함수끼리는 테이블·클래스·캡처 변수를 공유한다. 같은 Task를 여러 번 기다리면 같은 결과 객체를 받는다. native worker 경계에서는 기존처럼 입력·결과를 복사하며 userdata나 C++ 객체를 전달하지 않는다.

## 시작과 재개

async 함수는 호출한 자리에서 바로 시작해 첫 `await`까지 실행된다. JS의 async 함수, Unity의 `StartCoroutine`과 같다.

```lua
local async function hide(node: Node): ()
   node:setVisible(false)        -- hide(node)를 부른 그 자리에서 실행된다
   await scheduler.sleep(500)    -- 여기서 처음 멈추고, 호출한 쪽이 이어서 실행된다
   node:setVisible(true)         -- 500밀리초가 지난 뒤의 step에서 실행된다
end
```

- 한 번도 기다리지 않는 async 함수는 호출이 끝났을 때 이미 완료되어 있다. 완료된 Task의 `await`는 기다리지 않으므로 이런 호출은 step의 재개 횟수를 쓰지 않는다.
- 첫 `await` 전에 난 오류도 호출한 쪽으로 던지지 않는다. 호출은 실패한 Task를 돌려주고, 오류는 그 Task를 기다리는 쪽이 받는다.
- 기다린 뒤의 나머지는 스케줄러가 step에서 재개한다. `resolve`를 부르거나 타이머가 끝나도 기다리던 함수가 그 자리에서 실행되지는 않는다. 큐에 들어가 다음 재개 순서에 실행된다.
- 함수가 시작할 때는 호출 결과가 아직 변수에 들어가지 않았다. `local task; task = start()`에서 `start`의 본문은 첫 `await` 전까지 `task`를 nil로 본다.
- async 함수가 부른 async 함수는 그 안에서 시작한다. 이 중첩이 200단계를 넘은 호출은 큐에 넣어 다음 재개 순서에 시작한다. 재귀가 아무리 깊어도 C 스택을 넘기지 않게 하기 위해서다.

## 타이머

```lua
local async function blink(node: Node): ()
   node:setVisible(false)
   await scheduler.sleep(500)
   node:setVisible(true)
end
```

시간의 단위는 밀리초다. [I/O API](io.md)의 `timeout_ms`와 같다.

시계는 둘이다. **호스트 시간**은 호스트가 step에 넘긴 `elapsed`만큼 흐른다. 게임은 프레임 시간을 넘기므로 일시정지나 배속이 `scheduler.sleep`에 그대로 적용된다. **실제 시간**은 네이티브 모듈의 단조 시계를 따르며 `scheduler.sleep_realtime`이 쓴다. `elapsed` 없이 step하는 호스트와 CLI의 `scheduler.run`에서는 호스트 시간도 실제 시간을 따른다.

타이머는 step이 시작될 때 기한을 확인한다. 기한이 같은 타이머는 만든 순서대로 완료된다. step 도중에 만든 타이머는 다음 step부터 확인하므로 `sleep(0)`은 "다음 프레임"이다. 0 이상인 유한한 수만 받는다. 타이머 Task를 취소하면 기다리던 쪽은 취소 오류를 받는다. 네이티브 Task·I/O 모듈이 없는 순수 Lua 임베딩에서는 실제 시계가 없다. `elapsed`를 넘기는 호스트 시간 타이머만 쓸 수 있고 `sleep_realtime`과 타이머를 기다리는 `scheduler.run`은 오류다.

## 밖에서 완료시키는 Task

```lua
local async function wait_click(button: Button): string
   local task: Task<string>, resolve, reject = scheduler.deferred()
   button:addClickEventListener(function() resolve("clicked") end)
   return await task
end
```

`scheduler.deferred()`는 콜백·이벤트·호스트가 나중에 완료시키는 Task를 만든다. `resolve(...)`는 그 값들로 완료하고 `reject(error)`는 그 오류 값으로 실패시킨다. 둘 다 그 호출이 Task를 완료했을 때만 true를 돌려주며, 이미 완료·실패·취소된 Task에는 아무 일도 하지 않는다. 기다리던 async 함수는 다음 재개 순서에 이어서 실행된다.

Task의 타입은 `from_worker`처럼 `Task<any>`다. 위처럼 `Task<string>`으로 표기하면 그 계약으로 검사하지만, 실제로 넘긴 값이 맞는지는 실행 시 확인하지 않는다. 타입이 있는 코드의 await는 첫 값을 받는다. 아무도 완료시키지 않은 Task는 계속 pending으로 남고 `scheduler.run`에서는 교착 오류가 된다. `resolve`·`reject`는 그 Lua 상태를 소유한 스레드에서 부른다.

C++ 호스트는 [`arky::runtime::deferred`](embedding.md#호스트가-완료시키는-task)로 같은 일을 한다.

## 여러 Task 기다리기

```lua
local async function load(names: {string}): {Texture}
   local jobs: {Task<Texture>} = {}
   for _, name in ipairs(names) do jobs[#jobs + 1] = load_texture(name) end
   return await scheduler.all(jobs)
end

-- 시간 안에 답이 없으면 nil
local async function answer_within(question: Task<string>, milliseconds: number): string?
   local answer: string? = await scheduler.race({ question, scheduler.sleep(milliseconds) })
   return answer
end
```

| API | 끝나는 때 | 값 |
| --- | --- | --- |
| `scheduler.all(tasks)` | 모든 Task가 성공했을 때. 하나라도 실패하거나 취소되면 곧바로 그 오류로 실패 | 각 Task의 첫 값을 넘긴 순서대로 담은 배열 |
| `scheduler.race(tasks)` | 가장 먼저 끝난 Task가 끝났을 때. 그 Task가 실패했으면 같은 오류로 실패 | 그 Task의 모든 값 |

- 인자는 Task의 배열 하나다. 같은 Task를 여러 번 넣어도 된다. 빈 배열의 `all`은 바로 완료되고 `race`는 오류다.
- 넘겨받은 Task를 취소하지 않는다. `race`에서 진 Task는 계속 진행하고, 합친 Task를 취소해도 넘겨받은 Task들은 남는다. 그만두게 하려면 직접 `cancel()`한다.
- 값이 없는 Task(`Task<>`, 타이머)는 `all`의 배열에서 그 자리가 nil이다. 타입도 여기에 맞춰진다. 위의 `race` 결과가 `string?`인 것은 타이머가 먼저 끝나면 값이 없기 때문이다.
- 이미 끝난 Task가 여럿이면 `race`는 배열에서 가장 앞선 것을 따른다. 누가 먼저 끝났는지는 각 Task의 `ready()`·`status()`로 확인한다.

## 오류와 취소

실패한 Task의 await·result·run은 원래 Lua 오류 값을 전달한다. 문자열뿐 아니라 table·false·nil도 유지하며 `try/except/else`로 처리할 수 있다. 실패를 관찰하려면 Task를 기다리거나 result를 읽는다. step 자체가 모든 Task의 실패를 던지지는 않는다.

취소한 coroutine은 대기 목록에서 빠지고 다시 재개하지 않는다. 기다리던 작업을 취소해도 공유 의존 작업은 취소하지 않는다. worker 변환 핸들의 명시적 취소는 네이티브 핸들에도 취소를 요청한다. 변환 핸들은 즉시 cancelled가 되지만 실제 worker 종료는 기존 checkpoint 기반 협력 규칙을 따른다. 실행 중인 계산을 강제 선점하지 않는다. 순환 await와 실행 가능한 작업이 없는 교착은 오류로 처리한다.

## 호스트 루프 연결

호스트는 Lua 상태를 소유한 스레드에서 scheduler를 초기화하고 typed 코드와 같은 인스턴스를 사용해야 한다. 순수 Lua 임베딩의 최소 예다.

```lua
local runtime = assert(loadfile("runtime/async.lua"))()
local environment = setmetatable({
   __tynex_async_runtime = runtime, scheduler = runtime,
   fs = runtime.fs, net = runtime.net
}, {__index = _G})
local chunk = assert(loadfile("generated.lua"))
setfenv(chunk, environment)
chunk() -- async 진입 함수를 호출하는 코드. 본문은 여기서 첫 await까지 실행된다
-- 호스트의 매 프레임 콜백에서 같은 Lua 스레드로 호출. 두 번째 인자는 지난 프레임 시간(밀리초):
runtime.step(64, delta_seconds * 1000)
```

`arky run/debug/bundle`은 런타임 연결을 자동으로 준비한다. gen만 사용하는 호스트는 직접 주입해야 하며 static import가 있으면 전용 로더도 필요하다. 이렇게 직접 주입하는 호스트가 worker를 쓸 때는 초기화 전에 해당 엔진의 tynex_task 모듈 경로를 설정한다. C++ 호스트는 그 대신 [호스트 임베딩](embedding.md)의 `arky::runtime`으로 스케줄러·네이티브 모듈·import 로더를 한 번에 설치하고 프레임마다 `arky::runtime::step`을 부른다. 실제 Cocos Director/Scheduler 연결·앱 종료 처리는 호스트 통합 작업으로 남아 있다.

## 예제와 도구

```sh
./bin/arky run examples/async.arky
./bin/arky run examples/tasks/async.arky
./bin/arky run examples/timers.arky
python3 -m unittest discover -s tests -p 'test_async*.py' -v
```

VS Code·Zed 구문 강조, Task/await 진단, 원본 행·소스 맵을 지원한다. 런타임은 실패한 coroutine traceback을 보존하고 CLI 번들에서 원본 경로·행으로 매핑한다. 디버거의 전체 async 논리 스택·스케줄러 내부 프레임 숨김과 새 문법의 실제 GUI 세션은 후속 검증 대상이다.

파일 읽기·DNS·TCP 연결/송수신도 Task로 기다릴 수 있다. [I/O API](io.md)와 [실행 예제](../examples/io/README.md)를 따른다. HTTP/TLS는 아직 없다. 모바일은 [호스트 임베딩](embedding.md) 형태로 컴파일만 확인했고 기기 검증은 하지 않았다.

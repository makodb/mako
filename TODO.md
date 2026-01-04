# RustyCpp TODO
<!--
This comment block is the prompt content in case you forget.

Work on tasks defined in TODO.md. Repeat the following steps, don’t stop until interrupted. Don’t ask me for advice, just pick the best option you think that is honest, complete, and not corner-cutting: 

1. Pick the top undone task with highest priority (high-medium-low), choose its first leaf task.  If there are no undone TODO items left, sleep a minute and git pull and restart step 1 (so this step is a dead loop until you find a todo item).
2. Analyze the task, check if this can be done with not too many LOC (i.e., smaller than 500 lines code give or take). If not, try to analyze this task and break it down into several smaller tasks, expanding it in the TODO.md. The breakdown can be nested and hierarchical. Try to make each leaf task small enough (<500 lines LOC). You can document your analysis in the doc folder for future reference. 
3. Try to execute the first leaf task. Make a plan for the task before execute, put the plan in the docs folder, and add the file name in the item in TODO.md for reference. You can all write your key findings as a few sentences in the TODO item. When write code, you are only allowed to write rusty safe code following the rusty-cpp guidelines unless you are explicitly allowed by the todo item description. 
4. Make sure to add comprehensive test for the task executed. Run the whole test suites to make sure no regression happens (remember to use make clean && make -j32 because rusty-cpp requires make clean before build). If tests fail, fix them using the best, honest, complete approach, run test suites again to verify fixes work. Do not cheat such as disabling the borrow checker. Repeat this step until no tests fail. 
5. Prepare for git commit, remove all temporary files, especially not to commit any binary files. For plan files, extract from implementation plan the design rational and user manual and put it in the docs folder.
6. Git commit the changes. First do git pull --rebase, and fix conflicts if any. Remember to update submodule. If remote has any updates (merged through rebase), then run full ci tests again to make sure everything pass. If not pass, investigate and fix, repeat until pass all ci tests. Then do git push (if remote rejected because updates during we doing this step, restart this step).
7. Go back to step 1. (The TODO.md file is possibly updated, so make sure you read the updated TODO.)

-->

- [ ] Mako, build a high-performance, reliable, transactional, datastore; GA release
  - [ ] *high* build seems failing with most recent updates from rusty-cpp. make sure borrow-check is enabled for all files that have a safety annotation. investigate and fix the build failure.
  - [ ] *medium* Make rrr code naming following rust convention, e.g., class/types use UpperCamelCase, methods use snake_case. [Analysis: doc/naming_convention_analysis.md]
    - [x] reactor/event.h - Rename Event methods to snake_case (IsReady->is_ready, Test->test, Wait->wait, etc.) [DONE: commit d11bf085b]
    - [x] reactor/reactor.h - Rename Reactor methods to snake_case (GetReactor->get_reactor, Loop->loop, CreateSpEvent->create_sp_event, etc.) [DONE]
    - [x] reactor/coroutine.h - Rename Coroutine methods to snake_case (CreateRun->create_run, Yield->yield_, Continue->continue_, etc.) [DONE]
    - [x] reactor/quorum_event.h - Rename QuorumEvent methods to snake_case (VoteYes->vote_yes, VoteNo->vote_no, etc.) [DONE]
    - [x] base/threading.hpp - Already follows snake_case convention [DONE]
    - [x] misc/marshal.hpp - Rename Marshal/Marshallable methods to snake_case (ToMarshal->to_marshal, EntitySize->entity_size, etc.) [DONE]
    - [ ] misc/alock.hpp - Rename ALock methods to snake_case
    - [ ] rpc/*.hpp - Rename RPC-related methods to snake_case
    - [ ] Update all call sites throughout codebase for each renamed method
  - [ ] *medium* Make rrr code rusty-cpp safe. Expected results: only system calls and some really low-level code like memcpy are left in unsafe blocks, rest of the code are converted to rusty safe. During the process, there could be bug in rusty-cpp that causes issues. In this case, create a bug report in the rusty-cpp's docs' foder and add an entry to its TODO.md doc. git commit and push the report/todoenry to remote so that someone else will fix it. Monitor changes on the rusty-cpp by doing git pull in it every minute. Try again once it is updated. 

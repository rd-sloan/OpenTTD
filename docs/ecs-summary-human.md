
## Work done
- Created benchmarking testing beforehand to make sure we had numbers and observable values to check against on each phase.
	- Had two saves: Hilbergen and Wentbourne. Hilbergen was a more casual save, it contained ~2800 vehicle parts, all trains. Wentbourne is more of a 'push your CPU to the limit' save, with ~85,000 vehicle parts, of various types (mostly trains, but also included airplanes, road vehicles, etc).
	- Testing involved running headless for a specified number of frames and tracking stats. Most of this was already built into the codebase, but Claude did add some additional stats to track and added some additional testing to ensure that the end game state was the same, since the saves should be deterministic. 
- Kept the vehicle class as is, and slowly started moving logic out of it and into components. The vehicle class would create/destroy its corresponding entity and components during its ctor and dtor.
	- Priority was given to moving data that changed every frame into the components, 'cold' data that isn't accessed as often was largely ignored.
- Updated Tick Vehicles Loop to iterate over the registry instead of the list of vehicle class objects. 


## Results
- Overall, we did not increase perf. It's actually sizably worse.
	- It's possible if I kept going it could get better. There is still some hot data that hasn't been converted, so the TickVehicles loop is both iterating through the registry and also accessing the vehicle Class object (stored in a pool elsewhere). We aren't really getting gains from the densely packed registry because we are still fetching data from other places in memory every tick.
- I do wonder if a different refactoring approach would have been better.
	- As Claude said, "OpenTTD had already built the good half of an ECS". They were already getting benefits from having an object pool that is a densely packed slot map. 
	- Maybe it would have made more sense to focus on the "other half" of an ECS, like making systems that only touch specific subsets of the data, and modifying the existing pool to let us break things into components that are modified by those systems.
	- Or could have gone the way of still trying the EnTT registry, but instead of moving data slowly, start with the vehicles being one huge component, and then splitting data out into smaller components as we went.

## Notes about development with Claude
This was my first time using claude code and trying to prompt AI to refactor large amounts of code for me. There were some interesting things I noticed.

### The Plan
- I started off by having Claude inspect OpenTTD and then EnTT, and write a full plan on how we would integrate EnTT, using a phased approach.
	- After every 'phase' I would have claude re-evaluate and update the plan and log what we had done.
	- The ending document is basically a novel, not something I would want a human to have to read through end-to-end.
- At first, the plan seemed great. But as development went on, I would realize that while the plan was written very linearly, there were actually many secret dependencies and we could not just complete a phase as Claude had originally written it. We would need to start a later phase, or partially implement the current phase and come back to it later. 
- Knowing the codebase better probably would've helped prevent some of these issues, and I could have helped steer Claude into a possibly better plan after learning about the existing vehicle object pool already being densely packed.

### Iteration
I found that claude *loves* its rabbit holes.
- One consistent thing was it kept revising the best way to compare benchmark data. It seemed every phase it would notice something new and decide that the way we were measuring data needed more data, or a different process.
	- It would also sometimes run benchmarks in the background while simultaneously doing something else like updating documentation, which, as you would guess, could affect the performance of the benchmark. 
	- It eventually evolved to doing something like 3-5 runs each time it did a benchmark and then averaged out those values.
	- Sometimes this iteration felt a little overkill, but Claude didn't seem to understand the idea of "good enough".
- Another example is one time I asked why the train struct didn't change sizes after it removed a few members from it. That was a mistake. In every phase after that, it would keep checking the size and layout of the train struct again. It would insert temporary probes to get the exact layout of the struct, and then remove them. 
	- This is a case where I was just curious the first time, I didn't need it to be checked every single time after that. Saying "It's probably the layout" is good enough lol, but Claude wanted the precise answer.
- It randomly dumped the disassembly of the game to validate that the compiler actually devirtualized something. Seemed extremely overkill lol.
- It built an entire 'Shadow system' to validate that behavior wasn't changed as we moved logic into components. 
    - It used it once the first time it transferred variables, and did not use it when it updated all of the other variables.
- Watching it work step by step was interesting. 
	- A few times I saw it make up fake variables, get a compiler error and then correct itself, literally saying "Oh I made up that variable, let me make sure that any time I use a variable it actually exists in this other file."

### The human side of things
In general, I found it difficult to retain the info Claude gave me.
- I reviewed every bit claude was changing, as it was changing it, and then also did a more holistic code review after each 'phase' as well. I understood the changes it was making, but it was hard for me to remember/retain that information and I had to keep backtracking to documentation we had done to remember things. 
- As far as learning about EnTT, I got a general feel for the syntax and learned a few small things about it, but nothing I would feel confident regurgitating off the top of my head.
- I think I'm a better experiential learner - when I write the code and fumble myself I'm going to remember it much better.
- I definitely don't know the codebase well enough to ask Claude the right questions either. This was a lot of just hoping Claude was parsing code right, because I was just trying to learn about EnTT, not trying to learn in depth how OpenTTD works.

Also, sometimes I hate the way it speaks (I've heard this from others too lol)
- Especially in the summary. I asked it to make a short one, and I feel like its got this air of "things didn't work but it wasn't our fault and wasn't a waste of time" lmao. Or something. It just feels weird. Kind of like when you want a recipe but then they give you a life story before giving you the recipe. 

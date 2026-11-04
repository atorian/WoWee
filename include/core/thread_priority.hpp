#pragma once

namespace wowee::core {

/// Runs the calling thread below the threads that make the frame (the main
/// thread, the render thread, the frame workers): what loads the world -
/// tiles, textures, files - takes the cores the frame leaves.
///
/// Sixteen loading threads on a twelve-thread CPU took the main thread's time
/// while Dalaran loaded: a model name took 13 ms to classify, frames 130-530
/// ms. At the same priority the scheduler shares the cores evenly between
/// them; below it, a frame thread that is ready runs first.
///
/// Said once at warning level when the system refuses.
void runBelowFrameThreads();

}  // namespace wowee::core

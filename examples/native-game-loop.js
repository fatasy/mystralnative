import { gameLoop } from 'mystral/game-loop';

// Grand-strategy simulations usually need a stable logical cadence, not one
// simulation update for every rendered frame.
gameLoop.configure({
  simulationHz: 20,
  maxCatchUpTicks: 4,
  maxFrameDeltaMs: 250,
});

const world = { day: 0, previousDay: 0 };

gameLoop.setTickHandler(({ tick, deltaSeconds }) => {
  world.previousDay = world.day;
  world.day += deltaSeconds;

  // A simulation Worker can be driven from here with a synchronous postMessage:
  // simulationWorker.postMessage({ type: 'tick', tick, deltaSeconds });
  if (tick === 19) console.log('One simulated second completed');
});

gameLoop.start();

function render() {
  const { interpolationAlpha } = gameLoop.getState();
  const visibleDay = world.previousDay +
    (world.day - world.previousDay) * interpolationAlpha;

  // Render visibleDay with WebGPU/Three.js here.
  void visibleDay;
  requestAnimationFrame(render);
}

requestAnimationFrame(render);

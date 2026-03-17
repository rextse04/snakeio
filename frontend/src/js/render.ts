import GameState, {Point} from "./engine.ts";

export interface RenderContext {
    width: number;
    height: number;
    hex: HTMLCanvasElement | null | undefined;
    food: HTMLCanvasElement | null | undefined;
    snake: HTMLCanvasElement | null | undefined;
    map: HTMLCanvasElement | null | undefined;
}
export default function render(ctx: RenderContext, state: GameState, focus: Point | undefined){
    for (const canvas of [ctx.hex, ctx.food, ctx.snake, ctx.map]) {
        if (!canvas) continue;
        const canvasCtx = canvas.getContext("2d")!;
        canvasCtx.clearRect(0, 0, canvas.width, canvas.height);
    }

    const hexCtx = ctx.hex?.getContext("2d");
    const foodCtx = ctx.food?.getContext("2d");
    const snakeCtx = ctx.snake?.getContext("2d");
    const mapCtx = ctx.map?.getContext("2d");

    if (!focus) focus = {x: state.width / 2, y: state.height / 2};
    const offsetX = ctx.width / 2 - focus.x;
    const offsetY = ctx.height / 2 - focus.y;

    // Draw World Boundary
    if (hexCtx) {
        hexCtx.strokeStyle = "white";
        hexCtx.lineWidth = 2;
        hexCtx.strokeRect(offsetX, offsetY, state.width, state.height);
        hexCtx.fillStyle = "#17202A";
        hexCtx.fillRect(offsetX, offsetY, state.width, state.height);
    }

    // Draw Food
    if (foodCtx) {
        state.foods.forEach(food => {
            const screenX = food.pos.x + offsetX;
            const screenY = food.pos.y + offsetY;

            foodCtx.globalAlpha = 0.5;
            foodCtx.fillStyle = food.color;
            foodCtx.beginPath();
            foodCtx.arc(screenX, screenY, food.width, 0, 2 * Math.PI);
            foodCtx.fill();

            foodCtx.globalAlpha = 1;
            foodCtx.beginPath();
            foodCtx.arc(screenX, screenY, food.width / 2, 0, 2 * Math.PI);
            foodCtx.fill();
        });
    }

    // Draw Snakes
    if (snakeCtx) {
        state.snakes.forEach(snake => {
            if (!snake.alive) return;
            snakeCtx.fillStyle = snake.color;
            snake.segments.forEach((seg, i) => {
                const screenX = seg.x + offsetX;
                const screenY = seg.y + offsetY;

                snakeCtx.beginPath();
                snakeCtx.arc(screenX, screenY, snake.width, 0, 2 * Math.PI);
                snakeCtx.fill();

                if (i === 0) { // Head
                    // Draw eyes
                    snakeCtx.fillStyle = "white";
                    const eyeDist = snake.width * 0.7;
                    const eyeRadius = snake.width * 0.3;

                    // Left eye
                    const leX = screenX + eyeDist * Math.cos(snake.angle - 0.5);
                    const leY = screenY + eyeDist * Math.sin(snake.angle - 0.5);
                    snakeCtx.beginPath();
                    snakeCtx.arc(leX, leY, eyeRadius, 0, 2 * Math.PI);
                    snakeCtx.fill();

                    // Right eye
                    const reX = screenX + eyeDist * Math.cos(snake.angle + 0.5);
                    const reY = screenY + eyeDist * Math.sin(snake.angle + 0.5);
                    snakeCtx.beginPath();
                    snakeCtx.arc(reX, reY, eyeRadius, 0, 2 * Math.PI);
                    snakeCtx.fill();

                    // Pupils
                    snakeCtx.fillStyle = "black";
                    snakeCtx.beginPath();
                    snakeCtx.arc(
                        leX + Math.cos(snake.angle) * eyeRadius / 2,
                        leY + Math.sin(snake.angle) * eyeRadius / 2,
                        eyeRadius / 2, 0, 2 * Math.PI);
                    snakeCtx.fill();
                    snakeCtx.beginPath();
                    snakeCtx.arc(
                        reX + Math.cos(snake.angle) * eyeRadius / 2,
                        reY + Math.sin(snake.angle) * eyeRadius / 2,
                        eyeRadius / 2, 0, 2 * Math.PI);
                    snakeCtx.fill();

                    snakeCtx.fillStyle = snake.color;
                }
            });
        });
    }

    // Draw Map
    if (mapCtx) {
        const mapSize = {x: ctx.map!.width, y: ctx.map!.height};
        state.snakes.forEach((snake) => {
            if (!snake.alive || snake.segments.length === 0) return;
            const px = snake.segments[0].x / state.width * ctx.map!.width;
            const py = snake.segments[0].y / state.height * ctx.map!.height;
            mapCtx.fillStyle = snake.color;
            const dim = snake.width / 2;
            mapCtx.fillRect(px - dim / 2, py - dim / 2, dim, dim);
        });
        mapCtx.strokeStyle = "white";
        mapCtx.strokeRect(
            (focus.x - ctx.width / 2) / state.width * mapSize.x,
            (focus.y - ctx.height / 2) / state.height * mapSize.y,
            ctx.width / state.width * mapSize.x,
            ctx.height / state.height * mapSize.y);
    }
}
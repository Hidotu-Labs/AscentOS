#include <SDL2/SDL.h>
#include <stdio.h>

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    printf("Current video driver: %s\n", SDL_GetCurrentVideoDriver());

    SDL_Window* window = SDL_CreateWindow("AscentOS SDL2 Test",
                                          100, 100,
                                          640, 480,
                                          SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS);

    if (window == NULL) {
        printf("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    int num_drivers = SDL_GetNumRenderDrivers();
    printf("Number of render drivers: %d\n", num_drivers);
    for (int i = 0; i < num_drivers; i++) {
        SDL_RendererInfo info;
        SDL_GetRenderDriverInfo(i, &info);
        printf("Driver %d: %s (flags: 0x%X)\n", i, info.name, info.flags);
    }

    SDL_Renderer* renderer = NULL;
    
    // Try primary accelerated renderer
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL) {
        printf("Primary accelerated renderer failed: %s\n", SDL_GetError());
        // Try any accelerated renderer
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    }
    
    if (renderer == NULL) {
        printf("Accelerated renderer failed: %s\n", SDL_GetError());
        // Fallback to software renderer
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (renderer == NULL) {
        printf("Software renderer failed: %s\n", SDL_GetError());
        // Last resort: let SDL choose anything
        renderer = SDL_CreateRenderer(window, -1, 0);
    }

    if (renderer == NULL) {
        printf("All renderer attempts failed! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_RendererInfo info;
    SDL_GetRendererInfo(renderer, &info);
    printf("Successfully created renderer: %s\n", info.name);

    printf("Entering main loop. Press ESC to quit, or R/G/B to change color.\n");

    SDL_Event e;
    int quit = 0;
    int r = 100, g = 149, b = 237; // Cornflower Blue
    int frame_count = 0;

    while (!quit) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                quit = 1;
            } else if (e.type == SDL_KEYDOWN) {
                printf("Key pressed: %d\n", e.key.keysym.sym);
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        quit = 1;
                        break;
                    case SDLK_r: r = (r + 10) % 256; break;
                    case SDLK_g: g = (g + 10) % 256; break;
                    case SDLK_b: b = (b + 10) % 256; break;
                }
            }
        }

        if (SDL_SetRenderDrawColor(renderer, r, g, b, 255) != 0) {
            printf("SDL_SetRenderDrawColor failed: %s\n", SDL_GetError());
        }
        if (SDL_RenderClear(renderer) != 0) {
            printf("SDL_RenderClear failed: %s\n", SDL_GetError());
        }
        SDL_RenderPresent(renderer);

        frame_count++;
        if (frame_count % 100 == 0) {
            printf("Frame %d rendered (color: %d,%d,%d)\n", frame_count, r, g, b);
        }

        SDL_Delay(16);
    }

    printf("Exiting main loop.\n");

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

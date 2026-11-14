#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

typedef const unsigned char *(*get_string_fn)(unsigned int);
typedef void (*clear_color_fn)(float,float,float,float);
typedef void (*clear_fn)(unsigned int);
int main(int argc, char **argv) {
    const char *driver = argc > 1 ? argv[1] : "x11";
    SDL_SetHint(SDL_HINT_VIDEODRIVER, driver);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1; }
    printf("SDL %d.%d.%d driver=%s DISPLAY=%s WAYLAND_DISPLAY=%s\n",SDL_MAJOR_VERSION,SDL_MINOR_VERSION,SDL_PATCHLEVEL,SDL_GetCurrentVideoDriver(),getenv("DISPLAY")?getenv("DISPLAY"):"",getenv("WAYLAND_DISPLAY")?getenv("WAYLAND_DISPLAY"):"");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
    SDL_Window *window = SDL_CreateWindow("JianSSH SDL2 Zig glibc OpenGL3 smoke",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,640,360,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
    if (!window) { fprintf(stderr,"SDL_CreateWindow: %s\n",SDL_GetError()); SDL_Quit(); return 2; }
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) { fprintf(stderr,"SDL_GL_CreateContext: %s\n",SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return 3; }
    get_string_fn get_string=(get_string_fn)SDL_GL_GetProcAddress("glGetString");
    clear_color_fn clear_color=(clear_color_fn)SDL_GL_GetProcAddress("glClearColor");
    clear_fn clear=(clear_fn)SDL_GL_GetProcAddress("glClear");
    if (!get_string || !clear_color || !clear) { fprintf(stderr,"GL symbol lookup failed\n"); return 4; }
    printf("GL_VERSION=%s\nGL_RENDERER=%s\nGL_VENDOR=%s\n",get_string(0x1F02),get_string(0x1F01),get_string(0x1F00));
    Uint32 start=SDL_GetTicks(); unsigned frames=0; int running=1;
    while(running && SDL_GetTicks()-start<1800) {
        SDL_Event ev; while(SDL_PollEvent(&ev)) if(ev.type==SDL_QUIT) running=0;
        clear_color(0.08f,0.22f,0.35f,1.f); clear(0x4000); SDL_GL_SwapWindow(window); ++frames; SDL_Delay(16);
    }
    printf("PASS: visible OpenGL3 core window, frames=%u elapsed_ms=%u; clean exit\n",frames,SDL_GetTicks()-start);
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(window); SDL_Quit(); return 0;
}

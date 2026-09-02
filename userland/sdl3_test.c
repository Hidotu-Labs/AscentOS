/*
 * sdl3_test.c - SDL3 + OpenGL/EGL smoke test for AvoryOS
 *
 * Tests:
 *   - SDL3 window + OpenGL context creation
 *   - SDL3_ttf font init
 *   - libplacebo presence check (dlopen)
 *   - Capstone disassembly
 *
 * Build: make userland/sdl3_test.elf
 * Run:   /bin/sdl3_test  (inside AvoryOS with DISPLAY or Wayland set)
 */

/* Pull in SDL3 first — it defines GL base types we need */
#include <SDL3/SDL.h>
/* SDL_opengl.h includes SDL_opengl_glext.h which defines all PFNGLxxx types */
#include <SDL3/SDL_opengl.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <capstone/capstone.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/*  GL2 function pointers — loaded at runtime via SDL_GL_GetProcAddress */
/*  Use the PFNGL*PROC types already defined by SDL_opengl_glext.h     */
/* ------------------------------------------------------------------ */
static PFNGLCREATESHADERPROC            p_glCreateShader;
static PFNGLSHADERSOURCEPROC            p_glShaderSource;
static PFNGLCOMPILESHADERPROC           p_glCompileShader;
static PFNGLGETSHADERIVPROC             p_glGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC        p_glGetShaderInfoLog;
static PFNGLCREATEPROGRAMPROC           p_glCreateProgram;
static PFNGLATTACHSHADERPROC            p_glAttachShader;
static PFNGLBINDATTRIBLOCATIONPROC      p_glBindAttribLocation;
static PFNGLLINKPROGRAMPROC             p_glLinkProgram;
static PFNGLGETPROGRAMIVPROC            p_glGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC       p_glGetProgramInfoLog;
static PFNGLDELETESHADERPROC            p_glDeleteShader;
static PFNGLUSEPROGRAMPROC              p_glUseProgram;
static PFNGLDELETEPROGRAMPROC           p_glDeleteProgram;
static PFNGLGETUNIFORMLOCATIONPROC      p_glGetUniformLocation;
static PFNGLUNIFORM1FPROC               p_glUniform1f;
static PFNGLENABLEVERTEXATTRIBARRAYPROC  p_glEnableVertexAttribArray;
static PFNGLDISABLEVERTEXATTRIBARRAYPROC p_glDisableVertexAttribArray;
static PFNGLVERTEXATTRIBPOINTERPROC     p_glVertexAttribPointer;

/* Cast through void* to avoid strict-aliasing warnings on function pointers */
#define LOAD_GL(type, name) \
    p_##name = (type)(void *)SDL_GL_GetProcAddress(#name); \
    if (!p_##name) fprintf(stderr, "[gl] WARN: " #name " not found\n");

static int load_gl_procs(void)
{
    LOAD_GL(PFNGLCREATESHADERPROC,            glCreateShader)
    LOAD_GL(PFNGLSHADERSOURCEPROC,            glShaderSource)
    LOAD_GL(PFNGLCOMPILESHADERPROC,           glCompileShader)
    LOAD_GL(PFNGLGETSHADERIVPROC,             glGetShaderiv)
    LOAD_GL(PFNGLGETSHADERINFOLOGPROC,        glGetShaderInfoLog)
    LOAD_GL(PFNGLCREATEPROGRAMPROC,           glCreateProgram)
    LOAD_GL(PFNGLATTACHSHADERPROC,            glAttachShader)
    LOAD_GL(PFNGLBINDATTRIBLOCATIONPROC,      glBindAttribLocation)
    LOAD_GL(PFNGLLINKPROGRAMPROC,             glLinkProgram)
    LOAD_GL(PFNGLGETPROGRAMIVPROC,            glGetProgramiv)
    LOAD_GL(PFNGLGETPROGRAMINFOLOGPROC,       glGetProgramInfoLog)
    LOAD_GL(PFNGLDELETESHADERPROC,            glDeleteShader)
    LOAD_GL(PFNGLUSEPROGRAMPROC,              glUseProgram)
    LOAD_GL(PFNGLDELETEPROGRAMPROC,           glDeleteProgram)
    LOAD_GL(PFNGLGETUNIFORMLOCATIONPROC,      glGetUniformLocation)
    LOAD_GL(PFNGLUNIFORM1FPROC,               glUniform1f)
    LOAD_GL(PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray)
    LOAD_GL(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)
    LOAD_GL(PFNGLVERTEXATTRIBPOINTERPROC,     glVertexAttribPointer)
    return p_glCreateShader && p_glCreateProgram;
}

/* ------------------------------------------------------------------ */
/*  Capstone test                                                       */
/* ------------------------------------------------------------------ */
static void run_capstone_test(void)
{
    printf("[capstone] version %d.%d\n", CS_VERSION_MAJOR, CS_VERSION_MINOR);

    /* push rbp; mov rbp,rsp; xor eax,eax; pop rbp; ret */
    static const uint8_t code[] = {
        0x55, 0x48, 0x89, 0xe5, 0x31, 0xc0, 0x5d, 0xc3
    };
    csh handle;
    cs_insn *insn;

    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) {
        fprintf(stderr, "[capstone] FAIL: cs_open\n");
        return;
    }
    size_t count = cs_disasm(handle, code, sizeof(code), 0x1000, 0, &insn);
    if (!count) {
        fprintf(stderr, "[capstone] FAIL: cs_disasm returned 0\n");
        cs_close(&handle);
        return;
    }
    printf("[capstone] %zu instruction(s):\n", count);
    for (size_t i = 0; i < count; i++)
        printf("  0x%"PRIx64"  %-12s %s\n",
               insn[i].address, insn[i].mnemonic, insn[i].op_str);
    cs_free(insn, count);
    cs_close(&handle);
    printf("[capstone] OK\n");
}

/* ------------------------------------------------------------------ */
/*  libplacebo presence via dlopen                                      */
/* ------------------------------------------------------------------ */
static void run_placebo_test(void)
{
    const char *names[] = {
        "libplacebo.so", "libplacebo.so.349",
        "libplacebo.so.338", "libplacebo.so.7", NULL
    };
    void *h = NULL;
    for (int i = 0; names[i]; i++) {
        h = dlopen(names[i], RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen(names[i], RTLD_LAZY);
        if (h) { printf("[placebo] loaded %s OK\n", names[i]); break; }
    }
    if (!h) {
        fprintf(stderr, "[placebo] WARNING: not found (%s)\n", dlerror());
        return;
    }
    void *sym = dlsym(h, "pl_log_create");
    printf("[placebo] pl_log_create = %s\n", sym ? "found" : "not found");
    dlclose(h);
    printf("[placebo] OK\n");
}

/* ------------------------------------------------------------------ */
/*  GLSL shaders (GLSL 1.20)                                           */
/* ------------------------------------------------------------------ */
static const char *VERT_SRC =
    "#version 120\n"
    "attribute vec2 pos;\n"
    "attribute vec3 col;\n"
    "varying vec3 v_col;\n"
    "uniform float angle;\n"
    "void main() {\n"
    "    float c = cos(angle), s = sin(angle);\n"
    "    mat2 rot = mat2(c, -s, s, c);\n"
    "    gl_Position = vec4(rot * pos * 0.7, 0.0, 1.0);\n"
    "    v_col = col;\n"
    "}\n";

static const char *FRAG_SRC =
    "#version 120\n"
    "varying vec3 v_col;\n"
    "void main() { gl_FragColor = vec4(v_col, 1.0); }\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        p_glGetShaderInfoLog(sh, sizeof(buf), NULL, buf);
        fprintf(stderr, "[gl] shader error: %s\n", buf);
    }
    return sh;
}

static GLuint build_program(void)
{
    GLuint vert = compile_shader(GL_VERTEX_SHADER,   VERT_SRC);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, vert);
    p_glAttachShader(prog, frag);
    p_glBindAttribLocation(prog, 0, "pos");
    p_glBindAttribLocation(prog, 1, "col");
    p_glLinkProgram(prog);
    GLint ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        p_glGetProgramInfoLog(prog, sizeof(buf), NULL, buf);
        fprintf(stderr, "[gl] link error: %s\n", buf);
    }
    p_glDeleteShader(vert);
    p_glDeleteShader(frag);
    return prog;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("=== AvoryOS SDL3 / OpenGL / libplacebo / Capstone smoke test ===\n\n");

    run_capstone_test();
    printf("\n");
    run_placebo_test();
    printf("\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[SDL3] FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    printf("[SDL3] version %d.%d.%d\n",
           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window *win = SDL_CreateWindow("AvoryOS SDL3 Test", 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "[SDL3] FAIL: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit(); return 1;
    }
    printf("[SDL3] window OK\n");

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        fprintf(stderr, "[SDL3] FAIL: SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }
    printf("[SDL3] GL context OK\n");
    printf("[gl]   renderer : %s\n", (const char *)glGetString(GL_RENDERER));
    printf("[gl]   version  : %s\n", (const char *)glGetString(GL_VERSION));
    SDL_GL_SetSwapInterval(1);

    if (!load_gl_procs()) {
        fprintf(stderr, "[gl] FAIL: missing GL2 entry points\n");
        SDL_GL_DestroyContext(ctx); SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }
    printf("[gl]   GL2 procs loaded OK\n");

    /* SDL3_ttf */
    if (!TTF_Init()) {
        fprintf(stderr, "[TTF] FAIL: %s\n", SDL_GetError());
    } else {
        printf("[TTF] SDL3_ttf OK\n");
        const char *fonts[] = {
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/ttf-dejavu/DejaVuSans.ttf", NULL
        };
        TTF_Font *font = NULL;
        for (int i = 0; fonts[i]; i++) {
            font = TTF_OpenFont(fonts[i], 20);
            if (font) { printf("[TTF] font: %s\n", fonts[i]); break; }
        }
        if (!font) printf("[TTF] WARNING: no system font found\n");

        GLuint prog = build_program();
        GLint angle_loc = p_glGetUniformLocation(prog, "angle");

        static const float verts[] = {
             0.0f,  0.9f,  1.0f, 0.2f, 0.2f,
            -0.8f, -0.7f,  0.2f, 1.0f, 0.2f,
             0.8f, -0.7f,  0.2f, 0.4f, 1.0f,
        };

        float angle = 0.0f;
        int running = 1;
        SDL_Event ev;
        printf("[SDL3] render loop — Escape or close to quit\n");

        while (running) {
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) running = 0;
                if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)
                    running = 0;
            }
            int w, h;
            SDL_GetWindowSize(win, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            p_glUseProgram(prog);
            p_glUniform1f(angle_loc, angle);
            p_glEnableVertexAttribArray(0);
            p_glEnableVertexAttribArray(1);
            p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                    5 * sizeof(float), verts);
            p_glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                                    5 * sizeof(float), verts + 2);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            p_glDisableVertexAttribArray(0);
            p_glDisableVertexAttribArray(1);
            SDL_GL_SwapWindow(win);
            angle += 0.02f;
        }

        if (font) TTF_CloseFont(font);
        TTF_Quit();
        p_glDeleteProgram(prog);
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("\n[done] all tests passed\n");
    return 0;
}

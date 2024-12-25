#include <libdragon.h>
#include <math.h>
#include "setup.h"
#include "core.h"
#include "minigame.h"


/*=============================================================

=============================================================*/

#define POPTIME 0.4f
#define BLOCKSIZE  16
#define BLOCKSW    (320/BLOCKSIZE)
#define BLOCKSH    (240/BLOCKSIZE)
#define RECTCORNERDIST (BLOCKSIZE/1.41421356237)

#define FONTDEF_LARGE  1
#define FONTDEF_XLARGE 2

#define UNLIKELY(x) __builtin_expect(!!(x), 0)


/*=============================================================

=============================================================*/

typedef enum {
    MENU_START,
    MENU_MODE,
    MENU_PLAYERS,
    MENU_GAMESETUP,
    MENU_DONE,
} CurrentMenu;

typedef enum {
    TRANS_NONE,
    TRANS_FORWARD,
    TRANS_BACKWARD,
} Transition;

typedef enum {
    NR_LEAST = 0,
    NR_ROBIN = 1,
    NR_RANDOM = 2,
    NR_FULLRANDOM = 3,
} NextRound;


/*=============================================================

=============================================================*/

typedef struct {
    float h; // Angle (in degrees)
    float s; // 0 - 1
    float v; // 0 - 1
} hsv_t;

typedef struct {
    sprite_t* boxcorner;
    sprite_t* boxedge;
    sprite_t* boxback;
    surface_t boxedgesurface;
    surface_t boxbacksurface;
    int cornersize;
} BoxSpriteDef;

typedef struct {
    int w;
    int h;
    int x;
    int y;
    BoxSpriteDef spr;
} BoxDef;


/*=============================================================

=============================================================*/

static Transition global_transition;
static CurrentMenu global_curmenu;

static int global_firstport;
static int global_selection;
static bool global_playerjoined[MAXPLAYERS];
static float global_readyprog;
static bool  global_cursoractive;
static float global_cursory;
static bool  global_showsecond;

static int global_cfg_points;
static NextRound global_cfg_nextround;
static bool* global_cfg_blacklist;

static float global_backtime;
static float global_rotsel1;
static float global_rotsel2;
static float global_rotcursor;

static BoxSpriteDef sprdef_backbox;
static BoxSpriteDef sprdef_button;
static BoxDef* bdef_backbox_mode;
static BoxDef* bdef_backbox_plycount;
static BoxDef* bdef_backbox_aidiff;
static BoxDef* bdef_backbox_gameconfig;
static BoxDef* bdef_backbox_blacklist;
static BoxDef* bdef_button_freeplay;
static BoxDef* bdef_button_compete;
static sprite_t* spr_toybox;
static sprite_t* spr_trophy;
static sprite_t* spr_pointer;
static sprite_t* spr_robot;
static sprite_t* spr_player;
static sprite_t* spr_start;
static sprite_t* spr_a;
static sprite_t* spr_progress;
static sprite_t* spr_circlemask;
static rdpq_font_t* global_font1;
static rdpq_font_t* global_font2;


/*=============================================================

=============================================================*/

static void setup_draw(float deltatime);
static void drawbox(BoxDef* bd, color_t col);
static void drawprogress(int x, int y, color_t col);
static void drawfade(float time);
static void culledges(BoxDef* back);
static void uncull(void);
static bool is_menuvisible(CurrentMenu menu);


/*=============================================================

=============================================================*/

static inline float lerp(float from, float to, float frac)
{
    return from + (to - from)*frac;
}

static float elasticlerp(float from, float to, float frac)
{
    const float c4 = (2*M_PI)/3;
    if (frac <= 0)
        return from;
    if (frac >= 1)
        return to;
    const float ease = pow(2, -8.0f*frac)*sin((frac*8.0f - 0.75f)*c4) + 1;
    return from + (to - from)*ease;
}

static float deglerp(float from, float to, float frac)
{
    float delta = to - from;
    if (delta > 180)
        from += 360;
    else if (delta < -180)
        from -= 360;
    from = lerp(from, to, frac);
    if (from < 0)
        from += 360;
    return from;
}

static float clamp(float val, float min, float max)
{
    if (val < min)
        return min;
    if (val > max)
        return max;
    return val;
}

static hsv_t rgb2hsv(color_t rgb)
{
    hsv_t ret = {0, 0, 0};
    const float r = ((float)rgb.r)/255.0f;
    const float g = ((float)rgb.g)/255.0f;
    const float b = ((float)rgb.b)/255.0f;
    const float max = fmaxf(fmaxf(r, g), b);
    const float min = fminf(fminf(r, g), b);
    const float c = max - min;

    // Get value
    ret.v = max;

    // Get saturation
    if (ret.v != 0)
        ret.s = c/max;

    // Get hue
    if (c != 0)
    {
        if (max == r)
            ret.h = ((g - b)/c) + 0;
        else if (max == g)
            ret.h = ((b - r)/c) + 2;
        else
            ret.h = ((r - g)/c) + 4;
        ret.h *= 60;
        if (ret.h < 0)
            ret.h += 360;
    }
    return ret;
}

static color_t hsv2rgb(hsv_t hsv)
{
    float r, g, b;
    color_t ret = {0, 0, 0, 255};
    float h = hsv.h/360;
    int i = h*6;
    float f = h * 6 - i;
    float p = hsv.v*(1 - hsv.s);
    float q = hsv.v*(1 - f*hsv.s);
    float t = hsv.v*(1 - (1 - f)*hsv.s);
    
    switch (i % 6)
    {
        case 0: r = hsv.v; g = t;     b = p;     break;
        case 1: r = q;     g = hsv.v; b = p;     break;
        case 2: r = p;     g = hsv.v; b = t;     break;
        case 3: r = p;     g = q;     b = hsv.v; break;
        case 4: r = t;     g = p;     b = hsv.v; break;
        default:r = hsv.v; g = p;     b = q;     break;
    }
    
    ret.r = r*255;
    ret.g = g*255;
    ret.b = b*255;
    return ret;
}

static color_t lerpcolor(color_t from, color_t to, float frac)
{
    hsv_t result;
    hsv_t hsv_from = rgb2hsv(from);
    hsv_t hsv_to = rgb2hsv(to);
    result.h = deglerp(hsv_from.h, hsv_to.h, frac);
    result.s = lerp(hsv_from.s, hsv_to.s, frac);
    result.v = lerp(hsv_from.v, hsv_to.v, frac);
    return hsv2rgb(result);
}


/*=============================================================

=============================================================*/

void setup_init()
{
    const color_t plyclrs[] = {
        PLAYERCOLOR_1,
        PLAYERCOLOR_2,
        PLAYERCOLOR_3,
        PLAYERCOLOR_4,
    };
    global_selection = 0;
    global_transition = TRANS_FORWARD;
    global_curmenu = MENU_START;
    global_cursoractive = false;
    global_firstport = -1;
    for (int i=0; i<MAXPLAYERS; i++)
    {
        if (global_firstport == -1 && joypad_is_connected(i))
            global_firstport = i;
        global_playerjoined[i] = false;
    }

    global_backtime = 0;
    global_cursory = 0;
    global_rotsel1 = 0;
    global_rotsel2 = 0;
    global_rotcursor = 0;
    global_readyprog = 0;

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);

    global_cfg_points = 5;
    global_cfg_nextround = NR_LEAST;
    global_cfg_blacklist = (bool*)malloc(sizeof(bool)*global_minigame_count);
    for (int i=0; i<global_minigame_count; i++)
        global_cfg_blacklist[i] = false; // TODO: Read from save

    bdef_backbox_mode = (BoxDef*)malloc(sizeof(BoxDef));
    bdef_backbox_plycount = (BoxDef*)malloc(sizeof(BoxDef));
    bdef_backbox_aidiff = (BoxDef*)malloc(sizeof(BoxDef));
    bdef_backbox_gameconfig = (BoxDef*)malloc(sizeof(BoxDef));
    bdef_backbox_blacklist = (BoxDef*)malloc(sizeof(BoxDef));
    bdef_button_freeplay = (BoxDef*)malloc(sizeof(BoxDef));
    bdef_button_compete = (BoxDef*)malloc(sizeof(BoxDef));
    spr_toybox = sprite_load("rom:/core/ToyBox.rgba32.sprite");
    spr_trophy = sprite_load("rom:/core/Trophy.rgba32.sprite");
    spr_pointer = sprite_load("rom:/core/Pointer.rgba32.sprite");
    spr_player = sprite_load("rom:/core/Controller.rgba32.sprite");
    spr_robot = sprite_load("rom:/core/Robot.rgba32.sprite");
    spr_start = sprite_load("rom:/core/StartButton.sprite");
    spr_a = sprite_load("rom:/core/AButton.sprite");
    spr_progress = sprite_load("rom:/core/CircleProgress.i8.sprite");
    spr_circlemask = sprite_load("rom:/core/CircleMask.i8.sprite");
    global_font1 = rdpq_font_load("rom:/squarewave_l.font64");
    global_font2 = rdpq_font_load("rom:/squarewave_xl.font64");
    rdpq_text_register_font(FONTDEF_LARGE, global_font1);
    rdpq_text_register_font(FONTDEF_XLARGE, global_font2);
    rdpq_font_style(global_font1, 1, &(rdpq_fontstyle_t){.color =RGBA32(255, 255, 255, 255)});
    rdpq_font_style(global_font2, 1, &(rdpq_fontstyle_t){.color =RGBA32(255, 255, 255, 255)});
    rdpq_font_style(global_font1, 2, &(rdpq_fontstyle_t){.color =RGBA32(148, 145, 8, 255)}); // Do not use hard yellow due to Tritanopia
    for (int i=0; i<MAXPLAYERS; i++)
        rdpq_font_style(global_font1, 3+i, &(rdpq_fontstyle_t){.color = plyclrs[i]});

    sprdef_backbox.boxcorner = sprite_load("rom:/core/Box_Corner.rgba32.sprite");
    sprdef_backbox.boxedge = sprite_load("rom:/core/Box_Edge.rgba32.sprite");
    sprdef_backbox.boxback = sprite_load("rom:/pattern.i8.sprite");
    sprdef_backbox.boxedgesurface = sprite_get_pixels(sprdef_backbox.boxedge);
    sprdef_backbox.boxbacksurface = sprite_get_pixels(sprdef_backbox.boxback);
    sprdef_backbox.cornersize = 16;

    sprdef_button.boxcorner = sprite_load("rom:/core/Box2_Corner.rgba32.sprite");
    sprdef_button.boxedge = sprite_load("rom:/core/Box2_Edge.rgba32.sprite");
    sprdef_button.boxback = sprite_load("rom:/core/Box_Back.rgba32.sprite");
    sprdef_button.boxedgesurface = sprite_get_pixels(sprdef_button.boxedge);
    sprdef_button.boxbacksurface = sprite_get_pixels(sprdef_button.boxback);
    sprdef_button.cornersize = 8;

    bdef_backbox_mode->w = 0;
    bdef_backbox_mode->h = 0;
    bdef_backbox_mode->x = 320/2;
    bdef_backbox_mode->y = 240/2;
    bdef_backbox_mode->spr = sprdef_backbox;

    bdef_backbox_plycount->w = 280;
    bdef_backbox_plycount->h = 200;
    bdef_backbox_plycount->x = bdef_backbox_plycount->w*2;
    bdef_backbox_plycount->y = 240/2;
    bdef_backbox_plycount->spr = sprdef_backbox;

    bdef_backbox_aidiff->w = 0;
    bdef_backbox_aidiff->h = 0;
    bdef_backbox_aidiff->x = 320/2;
    bdef_backbox_aidiff->y = 240/2;
    bdef_backbox_aidiff->spr = sprdef_button;

    bdef_backbox_gameconfig->w = 280;
    bdef_backbox_gameconfig->h = 200;
    bdef_backbox_gameconfig->x = bdef_backbox_gameconfig->w*2;
    bdef_backbox_gameconfig->y = 240/2;
    bdef_backbox_gameconfig->spr = sprdef_backbox;

    bdef_backbox_blacklist->w = 0;
    bdef_backbox_blacklist->h = 0;
    bdef_backbox_blacklist->x = 320/2;
    bdef_backbox_blacklist->y = 240/2;
    bdef_backbox_blacklist->spr = sprdef_button;

    bdef_button_freeplay->w = 128;
    bdef_button_freeplay->h = 40;
    bdef_button_freeplay->spr = sprdef_button;

    bdef_button_compete->w = 128;
    bdef_button_compete->h = 40;
    bdef_button_compete->spr = sprdef_button;
}

void setup_loop(float deltatime)
{
    int maxselect = 0;
    joypad_buttons_t btns[4] = {0};
    for (int i=0; i<MAXPLAYERS; i++)
        btns[i] = joypad_get_buttons_pressed(i);

    // Handle controls
    switch (global_curmenu)
    {
        case MENU_START:
        {
            if (global_transition == TRANS_FORWARD && bdef_backbox_mode->w >= 270)
            {
                global_selection = 0;
                global_transition = TRANS_NONE;
                global_curmenu = MENU_MODE;
                global_cursory = bdef_button_freeplay->y - 12;
                global_cursoractive = true;
            }
            break;
        }
        case MENU_MODE:
        {
            maxselect = 2;

            if (global_cursoractive)
            {
                if (btns[global_firstport].a)
                {
                    global_selection = 0;
                    global_curmenu = MENU_PLAYERS;
                    global_transition = TRANS_FORWARD;
                    global_cursoractive = false;
                    global_showsecond = false;
                }
            }
            break;
        }
        case MENU_PLAYERS:
        {
            maxselect = 0;

            if (global_transition == TRANS_FORWARD && bdef_backbox_plycount->x <= 170)
            {
                global_cursoractive = true;
                global_transition = TRANS_NONE;
            }

            if (global_cursoractive)
            {
                bool gotonext = false;
                int firstcont = -1;
                if (!global_showsecond)
                    for (int i=0; i<MAXPLAYERS; i++)
                        if (btns[i].start)
                            global_playerjoined[i] = !global_playerjoined[i];
                for (int i=0; i<MAXPLAYERS; i++)
                    if (global_playerjoined[i] && firstcont == -1)
                        firstcont = i;
                if (!global_showsecond)
                {
                    if (firstcont != -1 && joypad_get_buttons(firstcont).a)
                    {
                        global_readyprog += deltatime;
                        if (global_readyprog >= 1)
                        {
                            int count = 0;
                            global_firstport = firstcont;
                            if (!global_playerjoined[MAXPLAYERS-1])
                            {
                                global_showsecond = true;
                                global_cursoractive = false;
                                global_selection = 1;
                            }
                            else
                                gotonext = true;
                            for (int i=0; i<MAXPLAYERS; i++)
                                if (global_playerjoined[i])
                                    count++;
                            core_set_playercount(count); // TODO: Modify core to allow different ports for p1
                        }
                    }
                    else
                        global_readyprog = 0;
                }
                else if (btns[global_firstport].a)
                {
                    gotonext = true;
                    core_set_aidifficulty(global_selection);
                }
                if (gotonext)
                {
                    global_readyprog = 0;
                    global_curmenu = MENU_GAMESETUP;
                    global_transition = TRANS_FORWARD;
                    global_cursoractive = false;
                }
            }
            break;
        }
        case MENU_GAMESETUP:
        {
            maxselect = 3;

            if (global_transition == TRANS_FORWARD && bdef_backbox_gameconfig->x <= 170)
            {
                global_selection = 0;
                global_cursoractive = true;
                global_transition = TRANS_NONE;
            }

            if (global_cursoractive)
            {
                if (btns[global_firstport].a || btns[global_firstport].d_right || btns[global_firstport].c_right)
                {
                    if (global_selection == 0)
                    {
                        global_cfg_points++;
                        if (global_cfg_points > 10)
                            global_cfg_points = 1;
                    }
                    else if (global_selection == 1)
                    {
                        global_cfg_nextround++;
                        if (global_cfg_nextround > NR_FULLRANDOM)
                            global_cfg_nextround = NR_LEAST;
                    }
                    else if (global_selection == 2)
                    {

                    }
                }
                else if (btns[global_firstport].d_left || btns[global_firstport].c_left)
                {
                    if (global_selection == 0)
                    {
                        global_cfg_points--;
                        if (global_cfg_points < 1)
                            global_cfg_points = 10;
                    }
                    else if (global_selection == 1)
                    {
                        if (global_cfg_nextround == NR_LEAST)
                            global_cfg_nextround = NR_FULLRANDOM;
                        else
                            global_cfg_nextround--;
                    }
                    else if (global_selection == 2)
                    {
                        
                    }
                }

                if (joypad_get_buttons(global_firstport).start)
                {
                    global_readyprog += deltatime;
                    if (global_readyprog >= 1)
                    {
                        global_readyprog = 0;
                        global_curmenu = MENU_DONE;
                        global_transition = TRANS_FORWARD;
                        global_cursoractive = false;
                    }
                }
                else
                    global_readyprog = 0;
            }
            break;
        }
        default:
        {
            break;
        }
    }

    // Handle animations and transitions
    if (is_menuvisible(MENU_MODE))
    {
        bdef_backbox_mode->w = elasticlerp(0, 280, global_backtime - POPTIME);
        bdef_backbox_mode->h = elasticlerp(0, 200, global_backtime - POPTIME);
        if (global_curmenu == MENU_PLAYERS && global_transition == TRANS_FORWARD)
            bdef_backbox_mode->x = lerp(bdef_backbox_mode->x, -bdef_backbox_mode->w, 7*deltatime);

        if (global_selection == 0)
        {
            global_rotsel1 = lerp(global_rotsel1, sin(global_backtime*2)/3, 10*deltatime);
            global_rotsel2 = lerp(global_rotsel2, 0, 10*deltatime);
            global_cursory = lerp(global_cursory, bdef_button_freeplay->y - 12, 10*deltatime);
        }
        else
        {
            global_rotsel1 = lerp(global_rotsel1, 0, 10*deltatime);
            global_rotsel2 = lerp(global_rotsel2, sin(global_backtime*2)/3, 10*deltatime);
            global_cursory = lerp(global_cursory, bdef_button_compete->y - 12, 10*deltatime);
        }
    }
    if (is_menuvisible(MENU_PLAYERS))
    {
        if (global_curmenu == MENU_PLAYERS && global_transition != TRANS_BACKWARD)
            bdef_backbox_plycount->x = lerp(bdef_backbox_plycount->x, 320/2, 7*deltatime);
        else if (global_curmenu == MENU_MODE && global_transition == TRANS_BACKWARD)
            bdef_backbox_plycount->x = lerp(bdef_backbox_plycount->x, 320+bdef_backbox_mode->w, 7*deltatime);
        else if (global_curmenu == MENU_GAMESETUP && global_transition == TRANS_FORWARD)
            bdef_backbox_plycount->x = lerp(bdef_backbox_plycount->x, -bdef_backbox_mode->w, 7*deltatime);
        if (global_showsecond)
        {
            bdef_backbox_aidiff->w = lerp(bdef_backbox_aidiff->w, 128, 10*deltatime);
            bdef_backbox_aidiff->h = lerp(bdef_backbox_aidiff->h, 128, 10*deltatime);
            if (bdef_backbox_aidiff->w > 100 && global_transition == TRANS_NONE)
            {
                global_cursoractive = true;
                maxselect = 3;
            }
        }
    }
    if (is_menuvisible(MENU_GAMESETUP))
    {
        if (global_curmenu == MENU_GAMESETUP && global_transition != TRANS_BACKWARD)
            bdef_backbox_gameconfig->x = lerp(bdef_backbox_gameconfig->x, 320/2, 7*deltatime);
        else if (global_curmenu == MENU_PLAYERS && global_transition == TRANS_BACKWARD)
            bdef_backbox_gameconfig->x = lerp(bdef_backbox_gameconfig->x, 320+bdef_backbox_mode->w, 7*deltatime);
        else if (global_curmenu == MENU_DONE && global_transition == TRANS_FORWARD)
        {
            bdef_backbox_gameconfig->w = lerp(bdef_backbox_gameconfig->w, 0, (1.0f)*deltatime);
            bdef_backbox_gameconfig->h = lerp(bdef_backbox_gameconfig->h, 0, (0.8f)*deltatime);
        }
    }

    // Handle parenting of objects to the main backbox
    bdef_button_freeplay->x = bdef_backbox_mode->x;
    bdef_button_freeplay->y = bdef_backbox_mode->y - 26;
    bdef_button_compete->x = bdef_backbox_mode->x;
    bdef_button_compete->y = bdef_backbox_mode->y + 26;
    bdef_backbox_aidiff->x = bdef_backbox_plycount->x;
    bdef_backbox_aidiff->y = bdef_backbox_plycount->y;

    // Handle cursor selection change
    if (global_cursoractive)
    {
        if (btns[global_firstport].d_down || btns[global_firstport].c_down)
        {
            global_selection++;
            if (global_selection >= maxselect)
                global_selection = 0;
        }
        else if (btns[global_firstport].d_up || btns[global_firstport].c_up)
        {
            global_selection--;
            if (global_selection < 0)
                global_selection = maxselect-1;
        }
    }

    // Draw the scene
    setup_draw(deltatime);
}

void setup_draw(float deltatime)
{
    const float backspeed = 0.3f;
    const color_t backcolors[] = {
        {255, 0,   0,   255},
        {255, 255, 0,   255},
        {0,   255, 0,   255},
        {0,   255, 255, 255},
        {0,   0,   255, 255},
        {255, 0,   255, 255},
    };
    rdpq_blitparms_t bp_freeplay = {.cx=16,.cy=16};
    rdpq_blitparms_t bp_compete = {.cx=16,.cy=16};
    int ccurr, cnext;

    // Increase background animation time
    global_backtime += deltatime;
    ccurr = ((int)(global_backtime*backspeed)) % (sizeof(backcolors)/sizeof(backcolors[0]));
    cnext = (ccurr + 1) % (sizeof(backcolors)/sizeof(backcolors[0]));

    // Begin drawing
    surface_t* disp = display_get();
    rdpq_attach(disp, NULL);

    // Draw the background
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(lerpcolor(backcolors[ccurr], backcolors[cnext], global_backtime*backspeed - floor(global_backtime*backspeed)));
    rdpq_fill_rectangle(0, 0, 320, 240);

    // Draw menu sprites
    if (is_menuvisible(MENU_MODE))
    {
        // Draw the container box
        drawbox(bdef_backbox_mode, (color_t){255, 255, 255, 255});
        culledges(bdef_backbox_mode);
            // Draw option buttons
            drawbox(bdef_button_freeplay, (color_t){200, 255, 200, 255});
            drawbox(bdef_button_compete, (color_t){255, 255, 200, 255});
            rdpq_set_prim_color((color_t){255, 255, 255, 255});

            // Draw button sprites
            if (global_selection == 0)
                bp_freeplay.theta = global_rotsel1;
            else
                bp_compete.theta = global_rotsel2;
            //rdpq_mode_filter(FILTER_BILINEAR);
            rdpq_sprite_blit(spr_toybox, bdef_button_freeplay->x + 40, bdef_button_freeplay->y, &bp_freeplay);
            rdpq_sprite_blit(spr_trophy, bdef_button_compete->x + 40, bdef_button_compete->y, &bp_compete);
        uncull();
    }
    if (is_menuvisible(MENU_PLAYERS))
    {
        int playernum = 0;
        const int sprsize = 32;
        const int padding = 16;
        drawbox(bdef_backbox_plycount, (color_t){255, 255, 255, 255});
        for (int i=0; i<MAXPLAYERS; i++)
        {
            if (global_playerjoined[i])
                rdpq_sprite_blit(spr_player, bdef_backbox_plycount->x +16 - (sprsize+padding)*2 + padding/2 + (sprsize+padding)*i, bdef_backbox_plycount->y, &(rdpq_blitparms_t){.cx=16, .cy=16, .scale_x=1+sin(global_backtime*15)/10, .scale_y=1+cos(global_backtime*15)/10});
            else
                rdpq_sprite_blit(spr_robot, bdef_backbox_plycount->x - (sprsize+padding)*2 + padding/2 + (sprsize+padding)*i, bdef_backbox_plycount->y-24, NULL);
        }
        rdpq_sprite_blit(spr_start, bdef_backbox_plycount->x - 48, bdef_backbox_plycount->y - 77, NULL);

        rdpq_text_print(&(rdpq_textparms_t){.width=320, .align=ALIGN_CENTER}, FONTDEF_XLARGE, bdef_backbox_plycount->x - 320/2, bdef_backbox_plycount->y-64, "Press    to join / quit");
        for (int i=0; i<MAXPLAYERS; i++)
        {
            if (global_playerjoined[i])
            {
                playernum++;
                drawprogress(bdef_backbox_plycount->x - 84, bdef_backbox_plycount->y + 48, RGBA32(255, 0, 0, 255));
                rdpq_sprite_blit(spr_a, bdef_backbox_plycount->x - 76, bdef_backbox_plycount->y+56, NULL);
                rdpq_text_printf(&(rdpq_textparms_t){.width=34, .align=ALIGN_CENTER, .style_id=2+playernum}, FONTDEF_LARGE, bdef_backbox_plycount->x - (sprsize+padding)*2 + padding/2 + (sprsize+padding)*i, bdef_backbox_plycount->y-30, "P%d", playernum);
            }
            else
                rdpq_text_print(&(rdpq_textparms_t){.width=34, .align=ALIGN_CENTER, .style_id=0}, FONTDEF_LARGE, bdef_backbox_plycount->x - (sprsize+padding)*2 + padding/2 + (sprsize+padding)*i, bdef_backbox_plycount->y-30, "CPU");
            if (playernum > 0)
                rdpq_text_print(&(rdpq_textparms_t){.width=320, .align=ALIGN_CENTER}, FONTDEF_LARGE, bdef_backbox_plycount->x - 320/2, bdef_backbox_plycount->y+68, "Hold      when everyone is ready");
        }
        if (global_showsecond)
        {
            drawbox(bdef_backbox_aidiff, (color_t){255, 255, 255, 255});
            culledges(bdef_backbox_aidiff);
                rdpq_set_prim_color(RGBA32(255, 255, 0, 255));
                rdpq_text_print(&(rdpq_textparms_t){.width=128, .align=ALIGN_CENTER, .char_spacing=1, .style_id=1}, FONTDEF_LARGE, bdef_backbox_aidiff->x - 64, bdef_backbox_aidiff->y - 40, "AI Difficulty");
                rdpq_text_print(&(rdpq_textparms_t){.width=128, .align=ALIGN_CENTER, .char_spacing=1, .style_id=((global_selection == 0) ? 2 : 1)}, FONTDEF_LARGE, bdef_backbox_aidiff->x - 64, bdef_backbox_aidiff->y - 10 + 24*0, "Easy");
                rdpq_text_print(&(rdpq_textparms_t){.width=128, .align=ALIGN_CENTER, .char_spacing=1, .style_id=((global_selection == 1) ? 2 : 1)}, FONTDEF_LARGE, bdef_backbox_aidiff->x - 64, bdef_backbox_aidiff->y - 10 + 24*1, "Medium");
                rdpq_text_print(&(rdpq_textparms_t){.width=128, .align=ALIGN_CENTER, .char_spacing=1, .style_id=((global_selection == 2) ? 2 : 1)}, FONTDEF_LARGE, bdef_backbox_aidiff->x - 64, bdef_backbox_aidiff->y - 10 + 24*2, "Hard");
            uncull();
        }
    }
    if (is_menuvisible(MENU_GAMESETUP))
    {
        // Draw the container box
        drawbox(bdef_backbox_gameconfig, (color_t){255, 255, 255, 255});
        culledges(bdef_backbox_gameconfig);
            drawprogress(bdef_backbox_gameconfig->x - 28 - 8, bdef_backbox_gameconfig->y + 60 - 8, RGBA32(0, 0, 255, 255));
            rdpq_sprite_blit(spr_start, bdef_backbox_gameconfig->x - 28, bdef_backbox_gameconfig->y+60, NULL);
        uncull();
    }

    // Pointer
    if (global_cursoractive)
    {
        global_rotcursor = lerp(global_rotcursor, cos(global_backtime*4)*8, 10*deltatime);
        rdpq_sprite_blit(spr_pointer, bdef_button_freeplay->x - bdef_button_freeplay->w + 28 + global_rotcursor, global_cursory, NULL);
    }

    // Draw z-buffered menu text (last due to render mode switches)
    if (is_menuvisible(MENU_MODE))
    {
        culledges(bdef_backbox_mode);
            rdpq_text_print(&(rdpq_textparms_t){.char_spacing=1, .style_id=1, .width=320, .align=ALIGN_CENTER}, FONTDEF_XLARGE, bdef_backbox_mode->x-320/2, bdef_backbox_mode->y-64, "I want to play:");
            rdpq_text_print(&(rdpq_textparms_t){.char_spacing=1, .style_id=1}, FONTDEF_LARGE, bdef_button_freeplay->x - 54, bdef_button_freeplay->y+4, "For fun!");
            rdpq_text_print(&(rdpq_textparms_t){.char_spacing=1, .style_id=1}, FONTDEF_LARGE, bdef_button_compete->x - 54, bdef_button_compete->y+4, "For glory!");
        uncull();
    }
    if (is_menuvisible(MENU_GAMESETUP))
    {
        const char* options[] = {
            "    Least Points",
            "    Round Robin",
            "    Random Player",
            "    Fully Random",
        };
        if (global_curmenu == MENU_DONE)
            culledges(bdef_backbox_gameconfig);
        rdpq_text_print(&(rdpq_textparms_t){.char_spacing=1, .style_id=1, .width=320, .align=ALIGN_CENTER}, FONTDEF_XLARGE, bdef_backbox_gameconfig->x-320/2, bdef_backbox_gameconfig->y-64, "Game Setup");
        rdpq_text_printf(&(rdpq_textparms_t){.char_spacing=1, .style_id=((global_cursoractive && global_selection == 0) ? 2 : 1)}, FONTDEF_LARGE, bdef_backbox_gameconfig->x-100, bdef_backbox_gameconfig->y-40, "Points to win: %d", global_cfg_points);
        rdpq_text_print(&(rdpq_textparms_t){.char_spacing=1, .style_id=((global_cursoractive && global_selection == 1) ? 2 : 1)}, FONTDEF_LARGE, bdef_backbox_gameconfig->x-100, bdef_backbox_gameconfig->y-10, "Who chooses next round: ");
        rdpq_text_print(&(rdpq_textparms_t){.char_spacing=1, .style_id=((global_cursoractive && global_selection == 1) ? 2 : 1)}, FONTDEF_LARGE, bdef_backbox_gameconfig->x-100, bdef_backbox_gameconfig->y+5, options[global_cfg_nextround]);
        rdpq_text_print(&(rdpq_textparms_t){.char_spacing=1, .style_id=((global_cursoractive && global_selection == 2) ? 2 : 1)}, FONTDEF_LARGE, bdef_backbox_gameconfig->x-100, bdef_backbox_gameconfig->y+35, "Modify minigame blacklist");
        rdpq_text_print(&(rdpq_textparms_t){.char_spacing=1, .style_id=1, .width=320, .align=ALIGN_CENTER}, FONTDEF_LARGE, bdef_backbox_gameconfig->x-320/2, bdef_backbox_gameconfig->y+72, "Hold      to finish");
        if (global_curmenu == MENU_DONE)
            uncull();
    }

    // Draw the screen wipe effect
    drawfade(global_backtime);

    // Done
    rdpq_detach_show();
}

void setup_cleanup()
{
    display_close();
    sprite_free(sprdef_backbox.boxcorner);
    sprite_free(sprdef_backbox.boxedge);
    sprite_free(sprdef_backbox.boxback);
    sprite_free(sprdef_button.boxcorner);
    sprite_free(sprdef_button.boxedge);
    sprite_free(sprdef_button.boxback);
    sprite_free(spr_toybox);
    sprite_free(spr_trophy);
    sprite_free(spr_pointer);
    sprite_free(spr_player);
    sprite_free(spr_robot);
    sprite_free(spr_start);
    sprite_free(spr_a);
    sprite_free(spr_progress);
    sprite_free(spr_circlemask);
    rdpq_text_unregister_font(FONTDEF_LARGE);
    rdpq_text_unregister_font(FONTDEF_XLARGE);
    rdpq_font_free(global_font1);
    rdpq_font_free(global_font2);
    free(bdef_backbox_mode);
    free(bdef_backbox_plycount);
    free(bdef_backbox_aidiff);
    free(bdef_backbox_gameconfig);
    free(bdef_backbox_blacklist);
    free(bdef_button_compete);
    free(bdef_button_freeplay);
}



/*=============================================================

=============================================================*/

static void drawbox(BoxDef* bd, color_t col)
{
    int w2, h2;
    if (bd->w < 32)
        bd->w = 32;
    if (bd->h < 32)
        bd->h = 32;
    w2 = bd->w/2;
    h2 = bd->h/2;

    // Initialize the drawing mode
    rdpq_set_mode_standard();
    rdpq_set_prim_color(col);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);

    // Background
    if (bd->spr.boxback != NULL)
    {
        int cornersizepad = bd->spr.cornersize - 6;
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0,0,PRIM,0), (TEX0,0,PRIM,0)));
        rdpq_tex_upload(TILE0, &bd->spr.boxbacksurface, &((rdpq_texparms_t){.s={.repeats=REPEAT_INFINITE},.t={.repeats=REPEAT_INFINITE}}));
        rdpq_texture_rectangle(TILE0, bd->x-w2+cornersizepad, bd->y-h2+cornersizepad, bd->x+w2-cornersizepad, bd->y+h2-cornersizepad, 0, 0);
    }
    else
    {
        int cornersizepad = bd->spr.cornersize - 6;
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_fill_rectangle(bd->x-w2+cornersizepad, bd->y-h2+cornersizepad, bd->x+w2-cornersizepad, bd->y+h2-cornersizepad);
    }

    // Corners
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0,0,PRIM,0), (TEX0,0,PRIM,0)));
    rdpq_sprite_blit(bd->spr.boxcorner, bd->x-w2, bd->y-h2, NULL);
    rdpq_sprite_blit(bd->spr.boxcorner, bd->x+w2-bd->spr.cornersize, bd->y-h2, &((rdpq_blitparms_t){.flip_x=true}));
    rdpq_sprite_blit(bd->spr.boxcorner, bd->x-w2, bd->y+h2-bd->spr.cornersize, &((rdpq_blitparms_t){.flip_y=true}));
    rdpq_sprite_blit(bd->spr.boxcorner, bd->x+w2-bd->spr.cornersize, bd->y+h2-bd->spr.cornersize, &((rdpq_blitparms_t){.flip_x=true, .flip_y=true}));

    // Edges
    if (bd->w > bd->spr.cornersize*2)
    {
        rdpq_tex_upload_sub(TILE0, &bd->spr.boxedgesurface, NULL, bd->spr.cornersize, 0, bd->spr.cornersize*2, bd->spr.cornersize);
        rdpq_texture_rectangle(TILE0, bd->x-w2+bd->spr.cornersize, bd->y-h2, bd->x+w2-bd->spr.cornersize, bd->y-h2+bd->spr.cornersize, 0, 0);
        rdpq_tex_upload_sub(TILE0, &bd->spr.boxedgesurface, NULL, 0, bd->spr.cornersize, bd->spr.cornersize, bd->spr.cornersize*2);
        rdpq_set_tile_size(TILE0, 0, 0, bd->spr.cornersize, bd->spr.cornersize);
        rdpq_texture_rectangle(TILE0, bd->x-w2+bd->spr.cornersize, bd->y+h2-bd->spr.cornersize, bd->x+w2-bd->spr.cornersize, bd->y+h2, 0, 0);
    }
    if (bd->h > bd->spr.cornersize*2)
    {
        rdpq_tex_upload_sub(TILE0, &bd->spr.boxedgesurface, NULL, 0, 0, bd->spr.cornersize, bd->spr.cornersize);
        rdpq_texture_rectangle(TILE0, bd->x-w2, bd->y-h2+bd->spr.cornersize, bd->x-w2+bd->spr.cornersize, bd->y+h2-bd->spr.cornersize, 0, 0);
        rdpq_tex_upload_sub(TILE0, &bd->spr.boxedgesurface, NULL, bd->spr.cornersize, bd->spr.cornersize, bd->spr.cornersize*2, bd->spr.cornersize*2);
        rdpq_set_tile_size(TILE0, 0, 0, bd->spr.cornersize, bd->spr.cornersize);
        rdpq_texture_rectangle(TILE0, bd->x+w2-bd->spr.cornersize, bd->y-h2+bd->spr.cornersize, bd->x+w2, bd->y+h2-bd->spr.cornersize, 0, 0);
    }
}

static void drawprogress(int x, int y, color_t col)
{
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_combiner(RDPQ_COMBINER2(
        (TEX1,0,PRIM,0),  (0,0,0,TEX0),
        (0,0,0,COMBINED), (0,0,0,TEX1)
    ));
    rdpq_set_prim_color(col);
    rdpq_mode_alphacompare((1.0f-global_readyprog)*255.0f);
    rdpq_tex_multi_begin();
        rdpq_sprite_upload(TILE0, spr_circlemask, NULL);
        rdpq_sprite_upload(TILE1, spr_progress, NULL);
    rdpq_tex_multi_end();
    rdpq_texture_rectangle(TILE0, x, y, x+32, y+32, 0, 0);
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0,0,PRIM,0), (TEX0,0,PRIM,0)));
}

static void culledges(BoxDef* back)
{
    int boxleft =  back->x - back->w/2 + back->spr.cornersize - 5;
    int boxtop =  back->y - back->h/2 + back->spr.cornersize - 5;
    int boxbottom =  back->y + back->h/2 - back->spr.cornersize + 5;
    int boxright =  back->x + back->w/2 - back->spr.cornersize + 5;
    if (boxleft < 0) boxleft = 0;
    if (boxtop < 0) boxtop = 0;
    if (boxright < boxleft) boxright = boxleft;
    if (boxbottom < boxtop) boxbottom = boxtop;

    rdpq_set_scissor(boxleft, boxtop, boxright, boxbottom);
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0,0,PRIM,0), (TEX0,0,PRIM,0)));
}

static void uncull(void)
{
    rdpq_set_scissor(0, 0, 320, 240);
}

static void drawfade(float time)
{
    if (time > 1)
        return;

    const float cornerdist = RECTCORNERDIST*3; // The bigger this value, the longer the trail that is left behind

    // Calculate the x and y intercepts of the perpendicular line to the diagonal of the frame
    // Since the travel is constant (and the line is perfectly diagonal), we can calculate it easily without needing to do trig
    const float perpendicularx1 = clamp(time-0.5, 0, 0.5)*2*320;
    const float perpendicularx2 = clamp(time,     0, 0.5)*2*320;
    const float perpendiculary1 = clamp(time,     0, 0.5)*2*240;
    const float perpendiculary2 = clamp(time-0.5, 0, 0.5)*2*240;

    // Prepare to draw a black fill rectangle
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(color_from_packed32(0x000000FF));

    // Edge case, avoid all the calculations below
    if (time < 0)
    {
        rdpq_fill_rectangle(0, 0, 320, 240);
        return;
    }

    // Draw each block
    for (int y=0; y<BLOCKSH; y++)
    {
        for (int x=0; x<BLOCKSW; x++)
        {
            const float blockx = x*BLOCKSIZE + BLOCKSIZE/2;
            const float blocky = y*BLOCKSIZE + BLOCKSIZE/2;
            const float distpointraw = ((blockx-perpendicularx1)*(-perpendiculary2+perpendiculary1) + (blocky-perpendiculary1)*(perpendicularx2-perpendicularx1));
            const float distpoint = distpointraw/sqrt((-perpendiculary2+perpendiculary1)*(-perpendiculary2+perpendiculary1) + (perpendicularx2-perpendicularx1)*(perpendicularx2-perpendicularx1));
            if (distpoint > -cornerdist)
            {
                float blocksize = clamp(-distpoint, -cornerdist, cornerdist)/cornerdist;
                blocksize = ((1-blocksize)/2)*BLOCKSIZE;
                rdpq_fill_rectangle(blockx - blocksize/2, blocky - blocksize/2, blockx + blocksize/2, blocky + blocksize/2);
            }
        }
    }
}

static bool is_menuvisible(CurrentMenu menu)
{
    switch (menu)
    {
        case MENU_MODE:
            return global_curmenu == MENU_START || global_curmenu == MENU_MODE || (global_curmenu == MENU_PLAYERS && global_transition == TRANS_FORWARD);
        case MENU_PLAYERS:
            return global_curmenu == MENU_PLAYERS || (global_curmenu == MENU_MODE && global_transition == TRANS_BACKWARD) || (global_curmenu == MENU_GAMESETUP && global_transition == TRANS_FORWARD);
        case MENU_GAMESETUP:
            return global_curmenu == MENU_GAMESETUP || (global_curmenu == MENU_PLAYERS && global_transition == TRANS_BACKWARD) || (global_curmenu == MENU_DONE && global_transition == TRANS_FORWARD);
        default:
            return false;
    }
}
/* Native layouts derived from design/ui-concepts; included by ui_menu.c. */
static int design_sidebar(void) {
    int theme=ui_theme_index();return theme==1||theme==3||theme==4||theme==5;
}
static int design_anim_progress(const ui_state_t *st) {
    /* 0 -> 100 over ~220 ms since the selection changed; 100 when settled
     * or never animated. */
    if (!st->focus_anim_start)
        return 100;
    unsigned long long el = menu_now_ms() - st->focus_anim_start;
    if (el >= 220)
        return 100;
    return (int)(el * 100 / 220);
}
static void design_art(ui_state_t *st,int index,ui_surface_t *s,int x,int y,int w,int h,int radius) {
    if(!strcasecmp(st->apps[index].name,"Desktop")) {
        ui_landscape(s,x,y,w,h,radius);
        if(h>300)ui_icon(s,x+w/2-60,y+h/2-40,120,0,0xfff4f9ff);
    } else ui_art_draw_round(st->server,st->apps[index].id,s,x,y,w,h,radius);
}
static void design_profile(ui_state_t *st,char *text,size_t size) {
    snprintf(text,size,"%dp / %d FPS target",st->cfg->stream.height,st->cfg->stream.fps);
}
/* when the selected game is ALREADY running on the
 * host (currentGame), the primary action is to resume it, not relaunch. */
static const char *design_action_label(ui_state_t *st) {
    if (st->server && st->server->currentGame && st->napps > 0 &&
        st->apps[st->sel_app].id == st->server->currentGame)
        return "Resume";
    return "Play";
}
static void design_play(ui_state_t *st,ui_surface_t *s,int x,int y,int width,int theme) {
    uint32_t fill=theme==1?COL_TEXT:theme==3?COL_ACCENT:theme==2?0xff96e5ff:theme==5?COL_ACCENT:0xff106676;
    uint32_t ink=theme==1?COL_BG:theme==2?0xff092443:theme==5?0xff0a1424:0xfff5fbff;
    ui_round_rect(s,x,y,width,72,theme==1?0:14,fill);
    if(theme==0||theme==4)ui_focus(s,x,y,width,72,14,COL_ACCENT,8);
    int center=x+width/2;ui_icon(s,center-74,y+22,30,3,ink);
    ui_label(s,center-25,y+10,36,1,ink,design_action_label(st));
}
static void design_footer(ui_state_t *st,ui_surface_t *s) {
    int mono=ui_theme_index()==1;
    int orbis=ui_theme_index()==5;
    ui_overlay(s,0,980,UI_W,100,COL_BG,ui_theme_index()==3?25:orbis?210:185);
    ui_rect(s,64,980,1792,1,COL_DIM);
    uint32_t cross=mono?COL_TEXT:0xff65bcff,back=mono?COL_TEXT:0xffff727d,refresh=mono?COL_TEXT:0xff69e4bd,square=mono?COL_TEXT:0xffb39bff;
    if(orbis) {
        /* PS4-style keycaps behind the button glyphs. */
        ui_round_rect(s,66,1006,50,50,10,COL_PANEL);
        ui_round_rect(s,269,1006,50,50,10,COL_PANEL);
        ui_round_rect(s,458,1006,50,50,10,COL_PANEL);
        if(st->tab==TAB_APPS&&st->napps)ui_round_rect(s,654,1006,50,50,10,COL_PANEL);
    }
    ui_icon(s,72,1014,38,7,cross);ui_label(s,124,1013,26,0,COL_TEXT,st->tab==TAB_APPS?design_action_label(st):st->tab==TAB_PCS?"Connect":"Edit");
    ui_icon(s,275,1014,38,6,back);ui_label(s,327,1013,26,0,COL_TEXT,"Back");
    ui_icon(s,464,1014,38,8,refresh);ui_label(s,516,1013,26,0,COL_TEXT,st->tab==TAB_SETTINGS?"Change":"Refresh");
    if(st->tab==TAB_APPS&&st->napps) {
        ui_icon(s,660,1014,38,9,square);ui_label(s,712,1013,26,0,COL_TEXT,"Default");
    }
    ui_round_rect(s,860,1014,57,38,5,COL_DIM);ui_round_rect(s,862,1016,53,34,4,COL_BG);
    ui_label(s,869,1014,23,0,COL_TEXT,"L1");ui_label(s,932,1014,23,0,COL_DIM,"/");
    ui_round_rect(s,959,1014,57,38,5,COL_DIM);ui_round_rect(s,961,1016,53,34,4,COL_BG);
    ui_label(s,968,1014,23,0,COL_TEXT,"R1");ui_label(s,1035,1013,26,0,COL_TEXT,"Tabs");
    char profile[80];design_profile(st,profile,sizeof(profile));
    ui_label(s,UI_W-64-ui_label_w(24,0,profile),1015,24,0,COL_DIM,profile);
}
static void design_shell(ui_state_t *st,ui_surface_t *s) {
    int theme=ui_theme_index(),side=design_sidebar();
    ui_theme_background(s);
    if(theme==4&&st->tab==TAB_APPS&&st->napps) {
        design_art(st,st->sel_app,s,0,0,UI_W,980,0);
        /* Fade the hero into readable navigation/title and thumbnail regions. */
        for(int x=0;x<UI_W;x+=8){int alpha=245-x*170/UI_W;ui_overlay(s,x,0,8,980,0xff030d15,alpha);}
        for(int y=580;y<980;y+=4)ui_overlay(s,0,y,UI_W,4,COL_BG,(y-580)*170/400);
    }
    int logo_size=theme==3?34:theme==1?48:42;
    /* the app is just "Moonlight" now - the old "PS4" corner tag is
     * gone from the header (the home-screen title matches). */
    ui_label(s,64,48,logo_size,theme!=1&&theme!=3,COL_TEXT,"MOONLIGHT");
    const char *tabs[]={"PCs","Games","Settings"};int icons[]={0,1,5};
    if(side) {
        int order[]={0,1,2};if(theme==1){order[0]=1;order[1]=0;}
        int rail=theme==3?390:theme==5?390:320;
        if(theme!=4)ui_rect(s,rail,theme==1?200:24,1,theme==1?740:930,COL_DIM);
        for(int i=0;i<3;i++) {
            int tab=order[i],y=250+i*118,selected=st->tab==tab;
            uint32_t ink=selected&&theme==1?COL_BG:selected&&(theme==3||theme==5)?(theme==5?COL_TEXT:0xfff8fbff):selected?COL_ACCENT:COL_DIM;
            if(selected) {
                if(theme==5) {
                    ui_round_rect(s,48,y-8,rail-72,80,12,COL_SEL);
                    ui_rect(s,48,y-8,4,80,COL_ACCENT);
                } else {
                    ui_round_rect(s,48,y-8,rail-72,80,theme==1?0:12,theme==1?COL_TEXT:theme==3?COL_ACCENT:COL_PANEL);
                    if(theme==4)ui_round_rect(s,48,y-8,4,80,2,COL_ACCENT);
                }
            }
            ui_icon(s,76,y+9,40,icons[tab],ink);ui_label(s,140,y+3,30,selected,ink,tabs[tab]);
        }
    } else {
        int y=theme==2?168:60,x0=theme==2?64:720;
        for(int i=0;i<3;i++) {
            int x=x0+i*245,selected=st->tab==i;
            if(theme==2)ui_round_rect(s,x,y-8,245,72,8,selected?COL_SEL:COL_PANEL);
            ui_label(s,x+40,y,32,selected,selected?COL_TEXT:COL_DIM,tabs[i]);
            if(selected){int width=theme==2?212:ui_label_w(32,1,tabs[i]);ui_rect(s,theme==2?x+16:x+40,y+53,width,3,COL_ACCENT);}
        }
    }
    int connected=st->server&&st->server->paired;
    if(theme==3||theme==5) {
        ui_rect(s,64,820,290,1,COL_DIM);ui_icon(s,70,857,44,0,COL_DIM);
        ui_label(s,132,840,26,1,COL_TEXT,"Gaming PC");ui_circle(s,138,896,6,connected?0xff42cc94:COL_WARN);
        ui_label(s,154,880,24,0,COL_DIM,connected?"Connected":"Disconnected");
        if(theme==5&&st->server&&st->server->currentGame) {
            const char *an=active_app_name(st);
            char chip[80];
            snprintf(chip,sizeof(chip),"Streaming: %s",an?an:"a game");
            ui_label_fit(s,70,918,21,0,COL_ACCENT,chip,310);
            ui_label(s,70,944,18,0,COL_DIM,"OPTIONS closes it");
        }
    } else if(theme!=4) {
        int hx=theme==1?1160:1500;
        if(theme!=1)ui_round_rect(s,hx-25,36,365,94,24,COL_PANEL);
        ui_icon(s,hx,58,42,0,COL_TEXT);ui_label(s,hx+66,42,26,0,COL_TEXT,"Gaming PC");
        ui_circle(s,hx+73,101,6,connected?0xff42dfaa:COL_WARN);
        ui_label(s,hx+90,85,22,0,connected?COL_ACCENT:COL_WARN,connected?"Connected":"Disconnected");
    }
    if(theme==1)ui_rect(s,64,174,1792,1,COL_DIM);
    if(theme==3||theme==5) {
        ui_label(s,440,48,48,1,COL_TEXT,tabs[st->tab]);
        char subtitle[180];
        if(st->tab==TAB_APPS)
            snprintf(subtitle,sizeof(subtitle),"%d games available on Gaming PC",st->napps);
        else if(st->tab==TAB_PCS)
            snprintf(subtitle,sizeof(subtitle),"Connect to Sunshine on your local network");
        else
            snprintf(subtitle,sizeof(subtitle),"Configure your streaming experience");
        ui_label(s,440,115,27,0,COL_DIM,subtitle);
    }
}
static void design_apps(ui_state_t *st,ui_surface_t *s) {
    int theme=ui_theme_index(),start_x=design_sidebar()?440:100;
    if(!st->napps) {
        if(st->refresh_pending) {
            ui_label(s,start_x,280,36,1,COL_TEXT,"Loading games...");
            ui_label(s,start_x,342,26,0,COL_DIM,"Fetching the list from your PC; this can take a few seconds.");return;
        }
        ui_label(s,start_x,280,36,1,COL_TEXT,st->apps_err?"Unable to load games":"No games available");
        ui_label(s,start_x,342,26,0,COL_DIM,"Press TRIANGLE to refresh, or check your PC connection.");return;
    }
    int count=theme==1?5:theme==2?6:theme==5?4:3;
    int first=st->sel_app/count*count;
    if(theme==1) {
        if(st->sel_app<st->app_first)st->app_first=st->sel_app;
        if(st->sel_app>=st->app_first+5)st->app_first=st->sel_app-4;
        first=st->app_first;
        ui_rect(s,1350,218,1,700,COL_DIM);
        for(int i=0;i<5&&first+i<st->napps;i++) {
            int idx=first+i,y=240+i*125,sel=idx==st->sel_app;
            ui_rect(s,354,y,950,105,COL_DIM);ui_rect(s,355,y+1,948,103,sel?COL_TEXT:COL_BG);
            uint32_t ink=sel?COL_BG:COL_TEXT;ui_icon(s,392,y+32,38,4,ink);
            ui_label_fit(s,478,y+22,36,sel,ink,st->apps[idx].name,720);
            if(sel)ui_icon(s,1240,y+34,36,3,ink);
        }
        ui_label_fit(s,1404,232,36,1,COL_TEXT,st->apps[st->sel_app].name,450);
        ui_rect(s,1404,320,450,1,COL_DIM);ui_icon(s,1410,364,44,0,COL_TEXT);
        ui_label(s,1480,352,28,0,COL_TEXT,"Gaming PC");ui_label(s,1480,400,24,0,COL_DIM,"Connected");
        ui_rect(s,1404,482,450,1,COL_DIM);design_play(st,s,1404,516,450,theme);
    } else if(theme==4) {
        ui_icon(s,380,252,44,0,COL_TEXT);ui_label(s,450,234,28,0,COL_TEXT,"Gaming PC");
        ui_circle(s,457,301,6,0xff49ecb8);ui_label(s,477,283,25,0,COL_ACCENT,"Connected");
        ui_label_fit(s,380,356,64,1,COL_TEXT,st->apps[st->sel_app].name,1390);
        design_play(st,s,380,483,360,theme);
        char profile[80];design_profile(st,profile,sizeof(profile));ui_icon(s,382,590,30,2,COL_DIM);ui_label(s,430,584,26,0,COL_DIM,profile);
        for(int i=0;i<3&&first+i<st->napps;i++) {
            int idx=first+i,x=64+i*610,y=708;
            design_art(st,idx,s,x,y,580,232,12);
            ui_overlay(s,x,y+154,580,78,COL_BG,195);
            ui_label_fit(s,x+22,y+172,28,1,COL_TEXT,st->apps[idx].name,540);
            if(idx==st->sel_app)ui_focus(s,x,y,580,232,12,COL_ACCENT,12);
        }
    } else if(theme==5) {
        /* Orbis: PS4 content row - four square tiles. Only the tile the
         * selection just MOVED TO eases into place (scale ~1.055 -> 1.00
         * over ~220 ms with a soft blue glow behind a crisp white ring);
         * the rest of the row stays perfectly still (previously every
         * tile grew/shrank on each step, which read as the whole row
         * jumping). Titles under every tile. */
        int t=design_anim_progress(st);
        for(int i=0;i<4&&first+i<st->napps;i++) {
            int idx=first+i,x=440+i*354,y=280,width=330,height=330,sel=idx==st->sel_app;
            int grow=sel?(100-t)*18/100:0; /* 0..18 px while settling */
            int fx=x-grow/2,fy=y-grow/2,fw=width+grow,fh=height+grow;
            design_art(st,idx,s,fx,fy,fw,fh,10);
            if(sel) {
                ui_focus(s,fx,fy,fw,fh,10,COL_ACCENT,14); /* soft glow */
                ui_focus(s,fx,fy,fw,fh,10,COL_TEXT,0);    /* white ring */
            }
            ui_label_fit(s,x,y+height+18,sel?30:22,sel,sel?COL_TEXT:COL_DIM,
                         st->apps[idx].name,width);
        }
        design_play(st,s,440,700,400,theme);
        char profile[80];design_profile(st,profile,sizeof(profile));
        ui_icon(s,448,802,26,2,COL_DIM);ui_label(s,486,796,24,0,COL_DIM,profile);
    } else if(theme==0) {
        for(int i=0;i<3&&first+i<st->napps;i++) {
            int idx=first+i,sel=idx==st->sel_app,x=64+i*608,y=198,width=570,height=594;
            design_art(st,idx,s,x,y,width,height,16);
            if(sel)ui_focus(s,x,y,width,height,16,COL_ACCENT,15);
            int title_width=ui_label_w(32,sel,st->apps[idx].name);
            int tx=x+(width-title_width)/2;if(tx<x+8)tx=x+8;
            ui_label_fit(s,tx,810,32,sel,sel?COL_TEXT:COL_DIM,st->apps[idx].name,width-16);
            if(sel)design_play(st,s,x+110,875,350,theme);
        }
    } else if(theme==2) {
        for(int i=0;i<6;i++) {
            int x=64+(i%3)*608,y=i<3?282:742,width=570,height=i<3?422:188;
            int idx=first+i,sel=idx==st->sel_app;
            if(idx>=st->napps){ui_round_rect(s,x,y,width,height,12,COL_PANEL);continue;}
            design_art(st,idx,s,x,y,width,height,12);
            if(sel)ui_focus(s,x,y,width,height,12,0xffdcfaff,16);
            if(i<3&&sel)design_play(st,s,x+14,y+height-82,width-28,theme);
            else {
                ui_overlay(s,x,y+height-67,width,67,COL_BG,180);
                ui_label_fit(s,x+24,y+height-54,30,1,COL_TEXT,st->apps[idx].name,width-48);
            }
        }
    } else { /* Daylight: two columns, persistent navigation and soft cards. */
        for(int i=0;i<3&&first+i<st->napps;i++) {
            int idx=first+i,sel=idx==st->sel_app,x=440+(i%2)*706,y=214+(i/2)*346,width=666,height=256;
            ui_round_rect(s,x+3,y+8,width,312,14,0xffe1ded8);
            ui_round_rect(s,x,y,width,310,12,COL_PANEL);
            design_art(st,idx,s,x,y,width,height,12);
            ui_rect(s,x,y+height-12,width,12,COL_PANEL);
            ui_label_fit(s,x+22,y+height+5,28,sel,COL_TEXT,st->apps[idx].name,width-44);
            if(sel)ui_focus(s,x,y,width,310,12,COL_ACCENT,0);
        }
        design_play(st,s,1166,572,626,theme);
        ui_rect(s,1166,676,626,1,COL_DIM);
        ui_label_fit(s,1166,700,26,1,COL_TEXT,st->apps[st->sel_app].name,626);
        ui_label(s,1166,748,24,0,COL_DIM,"Launch on your gaming PC and start streaming.");
        ui_rect(s,1166,809,626,1,COL_DIM);
        ui_icon(s,1170,840,32,0,COL_DIM);ui_label(s,1220,831,24,0,COL_DIM,"Gaming PC");
        char profile[80];design_profile(st,profile,sizeof(profile));
        ui_label(s,1166,884,24,0,COL_ACCENT,profile);
    }
    if(st->napps>count) {
        char position[80];
        int is_default=st->cfg->autostart&&atoi(st->cfg->app_name)==st->apps[st->sel_app].id;
        snprintf(position,sizeof(position),"%d / %d%s",st->sel_app+1,st->napps,is_default?"   AUTO-START":"");
        ui_label(s,design_sidebar()?440:64,942,22,0,is_default?COL_ACCENT:COL_DIM,position);
    } else if(st->napps) {
        int is_default=st->cfg->autostart&&atoi(st->cfg->app_name)==st->apps[st->sel_app].id;
        if(is_default)ui_label(s,design_sidebar()?440:64,942,22,0,COL_ACCENT,"AUTO-START");
    }
}

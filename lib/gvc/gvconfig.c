/* $Id$ $Revision$ */
/* vim:set shiftwidth=4 ts=8: */

/**********************************************************
*      This software is part of the graphviz package      *
*                http://www.graphviz.org/                 *
*                                                         *
*            Copyright (c) 1994-2004 AT&T Corp.           *
*                and is licensed under the                *
*            Common Public License, Version 1.0           *
*                      by AT&T Corp.                      *
*                                                         *
*        Information and Software Systems Research        *
*              AT&T Research, Florham Park NJ             *
**********************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>

#ifndef DISABLE_LTDL
#include	<sys/types.h>
#include	<sys/stat.h>
#include	<unistd.h>
#include	<glob.h>
#endif

#include        "types.h"
#include        "macros.h"
#include        "const.h"
#include        "graph.h"
#include	"gvplugin.h"
#include	"gvcint.h"
#include        "gvcproc.h"

#ifndef DISABLE_CODEGENS
#ifdef QUARTZ_RENDER
#include <QuickTime/QuickTime.h>

    extern codegen_t QPDF_CodeGen, QEPDF_CodeGen, QBM_CodeGen;
#endif
    extern codegen_t FIG_CodeGen, HPGL_CodeGen, MAP_CodeGen,
        MIF_CodeGen, XDot_CodeGen, MP_CodeGen, PIC_CodeGen,
        PS_CodeGen, DIA_CodeGen, SVG_CodeGen, VRML_CodeGen,
        VTX_CodeGen, GD_CodeGen, memGD_CodeGen;
#endif

/*
    A config for gvrender is a text file containing a
    list of plugin librariess and their capabilities using a tcl-like
    syntax

    Lines beginning with '#' are ignored as comments

    Blank lines are allowed and ignored.

    plugin_library_path packagename {
	plugin_api {
	    plugin_type plugin_quality
	    ...
	}
	...
    ...

    e.g.

	/usr/lib/graphviz/libgvplugin_cairo.so cairo {renderer {x 0 png 10 ps -10}}
	/usr/lib/graphviz/libgvplugin_gd.so gd {renderer {png 0 gif 0 jpg 0}}

    Internally the config is maintained as lists of plugin_types for each plugin_api.
    If multiple plugins of the same type are found then the highest quality wins.
    If equal quality then the last-one-installed wins (thus giving preference to
    external plugins over internal builtins).

 */

#ifndef DISABLE_LTDL
/*
  separator - consume all non-token characters until next token.  This includes:
	comments:   '#' ... '\n'
	nesting:    '{'
	unnesting:  '}'
	whitespace: ' ','\t','\n'

	*nest is changed according to nesting/unnesting processed
 */
static void separator(int *nest, char **tokens)
{
    char c, *s;

    s = *tokens;
    while ((c = *s)) {
	/* #->eol = comment */
	if (c == '#') {
	    s++;
	    while ((c = *s)) {
		s++;
		if (c == '\n')
		    break;
	    }
	    continue;
	}
	if (c == '{') {
	    (*nest)++;
	    s++;
	    continue;
	}
	if (c == '}') {
	    (*nest)--;
	    s++;
	    continue;
	}
	if (c == ' ' || c == '\n' || c == '\t') {
	    s++;
	    continue;
	}
	break;
    }
    *tokens = s;
}

/* 
  token - capture all characters until next separator, then consume separator,
	return captured token, leave **tokens pointing to next token.
 */
static char *token(int *nest, char **tokens)
{
    char c, *s, *t;

    s = t = *tokens;
    while ((c = *s)) {
	if (c == '#'
	    || c == ' ' || c == '\t' || c == '\n' || c == '{' || c == '}')
	    break;
	s++;
    }
    *tokens = s;
    separator(nest, tokens);
    *s = '\0';
    return t;
}

static int gvconfig_plugin_install_from_config(GVC_t * gvc, char *s)
{
    char *path, *packagename, *api, *type;
    api_t gv_api;
    int quality, rc;
    int nest = 0;

    separator(&nest, &s);
    while (*s) {
	path = token(&nest, &s);
	if (nest == 0)
	    packagename = token(&nest, &s);
        else
	    packagename = "x";
	do {
	    api = token(&nest, &s);
	    gv_api = gvplugin_api(api);
	    if (gv_api == -1) {
		agerr(AGERR, "invalid api in config: %s %s\n", path, api);
		return 0;
	    }
	    do {
		if (nest == 2) {
		    type = token(&nest, &s);
		    if (nest == 2)
		        quality = atoi(token(&nest, &s));
		    else
		        quality = 0;
		    rc = gvplugin_install (gvc, gv_api,
				    type, quality, packagename, path, NULL);
		    if (!rc) {
		        agerr(AGERR, "config error: %s %s %s\n", path, api, type);
		        return 0;
		    }
		}
	    } while (nest == 2);
	} while (nest == 1);
    }
    return 1;
}
#endif

static void gvconfig_plugin_install_from_library(GVC_t * gvc, char *path, gvplugin_library_t *library)
{
    gvplugin_api_t *apis;
    gvplugin_installed_t *types;
    int i;

    for (apis = library->apis; (types = apis->types); apis++) {
	for (i = 0; types[i].type; i++) {
	    gvplugin_install(gvc, apis->api, types[i].type,
			types[i].quality, library->packagename, path, &types[i]);
        }
    }
}

struct lt_symlist
{
    const char *name;
    void* address;
};

extern const struct lt_symlist lt_preloaded_symbols[];

static void gvconfig_plugin_install_builtins(GVC_t * gvc)
{
    const struct lt_symlist *s;
    const char *name;

    s = lt_preloaded_symbols;
    for (s = lt_preloaded_symbols; (name = s->name); s++)
	if (name[0] == 'g' && strstr(name, "_LTX_library")) 
	    gvconfig_plugin_install_from_library(gvc, NULL,
		    (gvplugin_library_t *)(s->address));
}

#ifndef DISABLE_LTDL
static void gvconfig_write_library_config(char *path, gvplugin_library_t *library, FILE *f)
{
    gvplugin_api_t *apis;
    gvplugin_installed_t *types;
    int i;

    fprintf(f, "%s %s {\n", path, library->packagename);
    for (apis = library->apis; (types = apis->types); apis++) {
        fprintf(f, "\t%s {\n", gvplugin_api_name(apis->api));
	for (i = 0; types[i].type; i++) {
	    fprintf(f, "\t\t%s %d\n", types[i].type, types[i].quality);
	}
	fputs ("\t}\n", f);
    }
    fputs ("}\n", f);
}

char * gvconfig_libdir(void)
{
    static char line[1024];
    static char *libdir;
    char *path, *tmp;
    FILE *f;

    if (!libdir) {

        /* this only works on linux, other systems will get GVLIBDIR only */
	libdir = GVLIBDIR;
        f = fopen ("/proc/self/maps", "r");
        if (f) {
            while (!feof (f)) {
	        if (!fgets (line, sizeof (line), f))
	            continue;
	        if (!strstr (line, " r-xp "))
	            continue;
		path = strchr (line, '/');
		if (!path)
		    continue;
		tmp = strstr (path, "/libgvc.");
		if (tmp) {
		    *tmp = 0;
		    libdir = path;
		    break;
	        }
            }
            fclose (f);
        }
    }
    return libdir;
}
#endif

#ifndef DISABLE_LTDL
static void config_rescan(GVC_t *gvc, char *config_path)
{
    FILE *f = NULL;
    glob_t globbuf;
    char *config_glob, *path, *libdir;
    int i, rc;
    gvplugin_library_t *library;
    char *plugin_glob = "libgvplugin*.so.?";

    if (config_path) {
	f = fopen(config_path,"w");
	if (!f) {
	    agerr(AGERR,"failed to open %s for write.\n", config_path);
	}
    }

    libdir = gvconfig_libdir();

    /* load all libraries even if can't save config */
    config_glob = malloc(strlen(libdir)
			    + 1
			    + strlen(plugin_glob)
			    + 1);
    strcpy(config_glob, libdir);
    strcat(config_glob, "/");
    strcat(config_glob, plugin_glob);

    rc = glob(config_glob, GLOB_NOSORT, NULL, &globbuf);
    if (rc == 0) {
	for (i = 0; i < globbuf.gl_pathc; i++) {
	    library = gvplugin_library_load(globbuf.gl_pathv[i]);
	    if (library) {
		gvconfig_plugin_install_from_library(gvc, globbuf.gl_pathv[i], library);
		path = strrchr(globbuf.gl_pathv[i],'/');
		if (path)
		    path++;
		if (f && path) {
		    gvconfig_write_library_config(path, library, f);
		}
	    }
	}
    }
    globfree(&globbuf);
    free(config_glob);
    if (f)
	fclose(f);
}
#endif

#ifndef DISABLE_CODEGENS

#define MAX_CODEGENS 100

static codegen_info_t cg[MAX_CODEGENS] = {
    {&PS_CodeGen, "ps", POSTSCRIPT},
    {&PS_CodeGen, "ps2", PDF},
    {&HPGL_CodeGen, "hpgl", HPGL},
    {&HPGL_CodeGen, "pcl", PCL},
    {&MIF_CodeGen, "mif", MIF},
    {&PIC_CodeGen, "pic", PIC_format},

    {&GD_CodeGen, "gd", GD},
#ifdef HAVE_LIBZ
    {&GD_CodeGen, "gd2", GD2},
#endif
#ifdef HAVE_GD_GIF
    {&GD_CodeGen, "gif", GIF},
#endif
#ifdef HAVE_GD_JPEG
    {&GD_CodeGen, "jpg", JPEG},
    {&GD_CodeGen, "jpeg", JPEG},
#endif
#ifdef HAVE_GD_PNG
    {&GD_CodeGen, "png", PNG},
    {&VRML_CodeGen, "vrml", VRML},
#endif
    {&GD_CodeGen, "wbmp", WBMP},
#ifdef HAVE_GD_XPM
    {&GD_CodeGen, "xbm", XBM},
    {&GD_CodeGen, "xpm", XBM},
#endif

#ifdef QUARTZ_RENDER
    {&QPDF_CodeGen, "pdf", QPDF},
    {&QEPDF_CodeGen, "epdf", QEPDF},
#endif                          /* QUARTZ_RENDER */

    {&MAP_CodeGen, "ismap", ISMAP},
    {&MAP_CodeGen, "imap", IMAP},
    {&MAP_CodeGen, "cmap", CMAP},
    {&MAP_CodeGen, "cmapx", CMAPX},
    {&VTX_CodeGen, "vtx", VTX},
    {&MP_CodeGen, "mp", METAPOST},
    {&FIG_CodeGen, "fig", FIG},
    {&SVG_CodeGen, "svg", SVG},
#ifdef HAVE_LIBZ
    {&SVG_CodeGen, "svgz", SVGZ},
    {&DIA_CodeGen, "dia", DIA},
#endif
#define DUMMY_CodeGen XDot_CodeGen
    {&DUMMY_CodeGen, "dot", ATTRIBUTED_DOT},
    {&DUMMY_CodeGen, "canon", CANONICAL_DOT},
    {&DUMMY_CodeGen, "plain", PLAIN},
    {&DUMMY_CodeGen, "plain-ext", PLAIN_EXT},
    {&DUMMY_CodeGen, "xdot", EXTENDED_DOT},
    {NULL, NULL, 0}
};

codegen_info_t *first_codegen(void)
{
    return cg;
}

codegen_info_t *next_codegen(codegen_info_t * p)
{
    ++p;

#ifdef QUARTZ_RENDER
    static boolean unscanned = TRUE;
    if (!p->name && unscanned) {
        /* reached end of codegens but haven't yet scanned for Quicktime codegens... */

        unscanned = FALSE;              /* don't scan again */

        ComponentDescription criteria;
        criteria.componentType = GraphicsExporterComponentType;
        criteria.componentSubType = 0;
        criteria.componentManufacturer = 0;
        criteria.componentFlags = 0;
        criteria.componentFlagsMask = graphicsExporterIsBaseExporter;

        codegen_info_t *next_cg;
        int next_id;
        Component next_component;

        /* make each discovered Quicktime format into a codegen */
        for (next_cg = p, next_id = QBM_FIRST, next_component =
             FindNextComponent(0, &criteria);
             next_cg < cg + MAX_CODEGENS - 1 && next_id <= QBM_LAST
             && next_component;
             ++next_cg, ++next_id, next_component =
             FindNextComponent(next_component, &criteria)) {
            next_cg->cg = &QBM_CodeGen;
            next_cg->id = next_id;
            next_cg->info = next_component;

            /* get four chars of extension, trim and convert to lower case */
            char extension[5];
            GraphicsExportGetDefaultFileNameExtension((GraphicsExportComponent) next_component, (OSType *) & extension);
            extension[4] = '\0';

            char *extension_ptr;
            for (extension_ptr = extension; *extension_ptr;
                 ++extension_ptr)
                *extension_ptr =
                    *extension_ptr == ' ' ? '\0' : tolower(*extension_ptr);
            next_cg->name = strdup(extension);
        }

        /* add new sentinel at end of dynamic codegens */
        next_cg->cg = (codegen_t *) 0;
        next_cg->id = 0;
        next_cg->info = (void *) 0;
        next_cg->name = (char *) 0;
    }
#endif
    return p;
}
#endif

/*
  gvconfig - parse a config file and install the identified plugins
 */
void gvconfig(GVC_t * gvc, boolean rescan)
{
#if 0
    gvplugin_library_t **libraryp;
#endif
#ifndef DISABLE_LTDL
    int sz, rc;
    struct stat config_st, libdir_st;
    FILE *f = NULL;
    char *config_path = NULL, *config_text = NULL;
    char *libdir;
    char *config_file_name = "config";

#define MAX_SZ_CONFIG 100000
#endif
    
#ifndef DISABLE_CODEGENS
    codegen_info_t *p;

    for (p = cg; p->name; ++p)
        gvplugin_install(gvc, API_render, p->name, 0,
                        "cg", NULL, (gvplugin_installed_t *) p);
#endif

#ifndef DISABLE_LTDL
    gvconfig_plugin_install_builtins(gvc);
   
    /* see if there are any new plugins */
    libdir = gvconfig_libdir();
    rc = stat(libdir, &libdir_st);
    if (rc == -1) {	/* if we fail to stat it then it probably doesn't exist
		   so just fail silently */
	return;
    }

    config_path = malloc(strlen(libdir) + 1 + strlen(config_file_name) + 1);
    strcpy(config_path, libdir);
    strcat(config_path, "/");
    strcat(config_path, config_file_name);
	
    if (rescan) {
	config_rescan(gvc, config_path);
    }
    else {
	/* load in the cached plugin library data */

    	rc = stat(config_path, &config_st);
	if (rc == -1) {
	    agerr(AGERR,"Unable to stat %s.\n", config_path);
	}
	else if (config_st.st_size > MAX_SZ_CONFIG) {
	    agerr(AGERR,"%s is bigger than I can handle.\n", config_path);
	}
	else {
	    f = fopen(config_path,"r");
	    if (!f) {
	        agerr (AGERR,"failed to open %s for read.\n", config_path);
	    }
	    else {
	        config_text = malloc(config_st.st_size + 1);
	        sz = fread(config_text, 1, config_st.st_size, f);
	        if (sz == 0) {
		    agerr(AGERR,"%s is zero sized, or other read error.\n", config_path);
		    free(config_text);
	        }
		else {
	            config_text[sz] = '\0';  /* make input into a null terminated string */
	            rc = gvconfig_plugin_install_from_config(gvc, config_text);
		    /* NB. config_text not freed because we retain char* into it */
		}
	    }
	    if (f)
		fclose(f);
	}
    }
    if (config_path)
	free(config_path);
#endif
}

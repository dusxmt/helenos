/*
 * Copyright (c) 2020 Jiri Svoboda
 * Copyright (c) 2012 Petr Koupy
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup launcher
 * @{
 */
/** @file Launcher
 */

#include <errno.h>
#include <gfx/coord.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <str.h>
#include <str_error.h>
#include <task.h>

#include <draw/surface.h>
#include <draw/source.h>
#include <draw/drawctx.h>
#include <draw/codec.h>

#include <ui/fixed.h>
#include <ui/image.h>
#include <ui/label.h>
#include <ui/pbutton.h>
#include <ui/resource.h>
#include <ui/ui.h>
#include <ui/window.h>

#include "images.h"
#include "launcher.h"

#define NAME  "launcher"

static char *display_spec = UI_DISPLAY_DEFAULT;

static void wnd_close(ui_window_t *, void *);

static ui_window_cb_t window_cb = {
	.close = wnd_close
};

static void pb_clicked(ui_pbutton_t *, void *);

static ui_pbutton_cb_t pbutton_cb = {
	.clicked = pb_clicked
};

static int app_launch(const char *);

/** Window close button was clicked.
 *
 * @param window Window
 * @param arg Argument (launcher)
 */
static void wnd_close(ui_window_t *window, void *arg)
{
	launcher_t *launcher = (launcher_t *) arg;

	ui_quit(launcher->ui);
}

/** Push button was clicked.
 *
 * @param pbutton Push button
 * @param arg Argument (launcher)
 */
static void pb_clicked(ui_pbutton_t *pbutton, void *arg)
{
	launcher_t *launcher = (launcher_t *) arg;

	if (pbutton == launcher->pb1) {
		app_launch("/app/terminal");
	} else if (pbutton == launcher->pb2) {
		app_launch("/app/calculator");
	} else if (pbutton == launcher->pb3) {
		app_launch("/app/uidemo");
	} else if (pbutton == launcher->pb4) {
		app_launch("/app/launcher");
	}
}

static int app_launch(const char *app)
{
	errno_t rc;
	task_id_t id;
	task_wait_t wait;

	if (display_spec != UI_DISPLAY_DEFAULT) {
		printf("%s: Spawning %s -d %s\n", NAME, app, display_spec);
		rc = task_spawnl(&id, &wait, app, app, "-d", display_spec, NULL);
	} else {
		printf("%s: Spawning %s\n", NAME, app);
		rc = task_spawnl(&id, &wait, app, app, NULL);
	}

	if (rc != EOK) {
		printf("%s: Error spawning %s %s (%s)\n", NAME, app,
		    display_spec != DISPLAY_DEFAULT ? display_spec :
		    "<default>", str_error(rc));
		return -1;
	}

	task_exit_t texit;
	int retval;
	rc = task_wait(&wait, &texit, &retval);
	if ((rc != EOK) || (texit != TASK_EXIT_NORMAL)) {
		printf("%s: Error retrieving retval from %s (%s)\n", NAME,
		    app, str_error(rc));
		return -1;
	}

	return retval;
}

static void print_syntax(void)
{
	printf("Syntax: %s [-d <display-spec>]\n", NAME);
}

int main(int argc, char *argv[])
{
	int i;
	launcher_t launcher;
	ui_t *ui = NULL;
	ui_wnd_params_t params;
	ui_window_t *window = NULL;
	ui_resource_t *ui_res;
	gfx_bitmap_params_t logo_params;
	gfx_bitmap_t *logo_bmp;
	gfx_bitmap_alloc_t alloc;
	gfx_context_t *gc;
	surface_coord_t w, h;
	gfx_rect_t logo_rect;
	gfx_rect_t rect;
	errno_t rc;

	i = 1;
	while (i < argc) {
		if (str_cmp(argv[i], "-d") == 0) {
			++i;
			if (i >= argc) {
				printf("Argument missing.\n");
				print_syntax();
				return 1;
			}

			display_spec = argv[i++];
		} else {
			printf("Invalid option '%s'.\n", argv[i]);
			print_syntax();
			return 1;
		}
	}

	surface_t *logo = decode_tga((void *) helenos_tga, helenos_tga_size,
	    SURFACE_FLAG_SHARED);
	if (!logo) {
		printf("Unable to decode logo.\n");
		return 1;
	}

	rc = ui_create(display_spec, &ui);
	if (rc != EOK) {
		printf("Error creating UI on display %s.\n", display_spec);
		return rc;
	}

	ui_wnd_params_init(&params);
	params.caption = "Launcher";
	params.rect.p0.x = 0;
	params.rect.p0.y = 0;
	params.rect.p1.x = 210;
	params.rect.p1.y = 300;

	memset((void *) &launcher, 0, sizeof(launcher));
	launcher.ui = ui;

	rc = ui_window_create(ui, &params, &window);
	if (rc != EOK) {
		printf("Error creating window.\n");
		return rc;
	}

	ui_window_set_cb(window, &window_cb, (void *) &launcher);
	launcher.window = window;

	ui_res = ui_window_get_res(window);
	gc = ui_window_get_gc(window);

	surface_get_resolution(logo, &w, &h);
	gfx_bitmap_params_init(&logo_params);
	logo_params.rect.p1.x = w;
	logo_params.rect.p1.y = h;
	logo_rect = logo_params.rect;

	alloc.pitch = sizeof(uint32_t) * w;
	alloc.off0 = 0;
	alloc.pixels = surface_direct_access(logo);

	rc = gfx_bitmap_create(gc, &logo_params, &alloc, &logo_bmp);
	if (rc  != EOK) {
		printf("Error creating bitmap.\n");
		return 1;
	}

	rc = ui_fixed_create(&launcher.fixed);
	if (rc != EOK) {
		printf("Error creating fixed layout.\n");
		return rc;
	}

	rc = ui_image_create(ui_res, logo_bmp, &logo_rect, &launcher.image);
	if (rc != EOK) {
		printf("Error creating label.\n");
		return rc;
	}

	rect.p0.x = 5;
	rect.p0.y = 32;
	rect.p1.x = 5 + w;
	rect.p1.y = 32 + h;
	ui_image_set_rect(launcher.image, &rect);

	rc = ui_fixed_add(launcher.fixed, ui_image_ctl(launcher.image));
	if (rc != EOK) {
		printf("Error adding control to layout.\n");
		return rc;
	}
	rc = ui_label_create(ui_res, "Launch application", &launcher.label);
	if (rc != EOK) {
		printf("Error creating label.\n");
		return rc;
	}

	rect.p0.x = 60;
	rect.p0.y = 107;
	rect.p1.x = 160;
	rect.p1.y = 120;
	ui_label_set_rect(launcher.label, &rect);
	ui_label_set_halign(launcher.label, gfx_halign_center);

	rc = ui_fixed_add(launcher.fixed, ui_label_ctl(launcher.label));
	if (rc != EOK) {
		printf("Error adding control to layout.\n");
		return rc;
	}

	rc = ui_pbutton_create(ui_res, "Terminal", &launcher.pb1);
	if (rc != EOK) {
		printf("Error creating button.\n");
		return rc;
	}

	ui_pbutton_set_cb(launcher.pb1, &pbutton_cb, (void *) &launcher);

	rect.p0.x = 15;
	rect.p0.y = 130;
	rect.p1.x = 190;
	rect.p1.y = 158;
	ui_pbutton_set_rect(launcher.pb1, &rect);

	rc = ui_fixed_add(launcher.fixed, ui_pbutton_ctl(launcher.pb1));
	if (rc != EOK) {
		printf("Error adding control to layout.\n");
		return rc;
	}

	rc = ui_pbutton_create(ui_res, "Calculator", &launcher.pb2);
	if (rc != EOK) {
		printf("Error creating button.\n");
		return rc;
	}

	ui_pbutton_set_cb(launcher.pb2, &pbutton_cb, (void *) &launcher);

	rect.p0.x = 15;
	rect.p0.y = 170;
	rect.p1.x = 190;
	rect.p1.y = 198;
	ui_pbutton_set_rect(launcher.pb2, &rect);

	rc = ui_fixed_add(launcher.fixed, ui_pbutton_ctl(launcher.pb2));
	if (rc != EOK) {
		printf("Error adding control to layout.\n");
		return rc;
	}

	rc = ui_pbutton_create(ui_res, "UI Demo", &launcher.pb3);
	if (rc != EOK) {
		printf("Error creating button.\n");
		return rc;
	}

	ui_pbutton_set_cb(launcher.pb3, &pbutton_cb, (void *) &launcher);

	rect.p0.x = 15;
	rect.p0.y = 210;
	rect.p1.x = 190;
	rect.p1.y = 238;
	ui_pbutton_set_rect(launcher.pb3, &rect);

	rc = ui_fixed_add(launcher.fixed, ui_pbutton_ctl(launcher.pb3));
	if (rc != EOK) {
		printf("Error adding control to layout.\n");
		return rc;
	}

	rc = ui_pbutton_create(ui_res, "Launcher", &launcher.pb4);
	if (rc != EOK) {
		printf("Error creating button.\n");
		return rc;
	}

	ui_pbutton_set_cb(launcher.pb4, &pbutton_cb, (void *) &launcher);

	rect.p0.x = 15;
	rect.p0.y = 250;
	rect.p1.x = 190;
	rect.p1.y = 278;
	ui_pbutton_set_rect(launcher.pb4, &rect);

	rc = ui_fixed_add(launcher.fixed, ui_pbutton_ctl(launcher.pb4));
	if (rc != EOK) {
		printf("Error adding control to layout.\n");
		return rc;
	}

	ui_window_add(window, ui_fixed_ctl(launcher.fixed));

	(void) ui_res;
	(void) app_launch;

	rc = ui_window_paint(window);
	if (rc != EOK) {
		printf("Error painting window.\n");
		return rc;
	}

	ui_run(ui);

	ui_window_destroy(window);
	ui_destroy(ui);

	return 0;
}

/** @}
 */

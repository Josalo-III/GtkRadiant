/*
   Copyright (C) 1999-2007 id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#ifndef INCLUDED_GTKUTIL_H
#define INCLUDED_GTKUTIL_H

#include <gtk/gtk.h>

inline int gtkutil_widget_get_width( GtkWidget* widget ){
	GtkAllocation allocation;
	gtk_widget_get_allocation( widget, &allocation );
	return allocation.width;
}

inline int gtkutil_widget_get_height( GtkWidget* widget ){
	GtkAllocation allocation;
	gtk_widget_get_allocation( widget, &allocation );
	return allocation.height;
}

#endif

#pragma once

#include "uvcsink.h"

void uvcsink_class_install_properties(UVCSinkClass *);
void uvcsink_set_property(GObject *, guint, const GValue *, GParamSpec *);
void uvcsink_get_property(GObject *, guint, GValue *, GParamSpec *);

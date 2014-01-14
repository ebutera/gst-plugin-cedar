# Note: plugindir is set in configure

plugin_LTLIBRARIES = libgstcedar.la

# sources used to compile this plug-in
libgstcedar_la_SOURCES = gstcedarh264enc.c gstcedarh264enc.h \
	ve.c ve.h

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstcedar_la_CFLAGS = $(GST_CFLAGS)
libgstcedar_la_LIBADD = $(GST_LIBS)
libgstcedar_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstcedar_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstcedarh264enc.h ve.h

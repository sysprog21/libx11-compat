#include <gtk/gtk.h>

static gboolean clicked = FALSE;

static gint expose_cb(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
    (void) event;
    (void) data;
    gdk_draw_rectangle(widget->window, widget->style->white_gc, TRUE, 24, 20,
                       172, 50);
    gdk_draw_rectangle(widget->window, widget->style->black_gc, FALSE, 24, 20,
                       172, 50);
    return TRUE;
}

static gint button_press_cb(GtkWidget *widget,
                            GdkEventButton *event,
                            gpointer data)
{
    (void) widget;
    (void) event;
    (void) data;
    clicked = TRUE;
    return TRUE;
}

static gint delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void) widget;
    (void) event;
    (void) data;
    return FALSE;
}

static void destroy(GtkWidget *widget, gpointer data)
{
    (void) widget;
    (void) data;
    gtk_main_quit();
}

int main(int argc, char **argv)
{
    GtkWidget *window;
    GtkWidget *drawing_area;

    gtk_init(&argc, &argv);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GTK1 Smoke");
    gtk_widget_set_usize(window, 220, 90);
    gtk_signal_connect(GTK_OBJECT(window), "delete_event",
                       GTK_SIGNAL_FUNC(delete_event), NULL);
    gtk_signal_connect(GTK_OBJECT(window), "destroy", GTK_SIGNAL_FUNC(destroy),
                       NULL);

    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_usize(drawing_area, 220, 90);
    gtk_widget_set_events(drawing_area,
                          GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK);
    gtk_signal_connect(GTK_OBJECT(drawing_area), "expose_event",
                       GTK_SIGNAL_FUNC(expose_cb), NULL);
    gtk_signal_connect(GTK_OBJECT(drawing_area), "button_press_event",
                       GTK_SIGNAL_FUNC(button_press_cb), NULL);
    gtk_container_add(GTK_CONTAINER(window), drawing_area);

    gtk_widget_show(drawing_area);
    gtk_widget_show(window);
    gtk_main();

    return clicked ? 0 : 2;
}

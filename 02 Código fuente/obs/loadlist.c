/*
 * Load list table in C using GTK4: GtkColumnView + GListStore.
 *
 * Build:
 *   gcc loadlist.c -o loadlist $(pkg-config --cflags --libs gtk4) -Wall
 *
 * Needs the GTK4 dev package:
 *   Debian/Ubuntu: sudo apt install libgtk-4-dev build-essential pkg-config
 *   Fedora:        sudo dnf install gtk4-devel gcc pkgconf
 *   Arch:          sudo pacman -S gtk4 base-devel
 *
 * Click a cell to edit it, Enter to confirm. Total columns update live.
 */
#include <gtk/gtk.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Row object: LoadItem                                                */
/* ------------------------------------------------------------------ */

typedef enum { F_NAME, F_DESC, F_QTY, F_WEIGHT, F_TOTAL } Field;

#define LOAD_TYPE_ITEM (load_item_get_type())
G_DECLARE_FINAL_TYPE(LoadItem, load_item, LOAD, ITEM, GObject)

struct _LoadItem {
    GObject parent_instance;
    char   *name;
    char   *desc;
    int     qty;
    double  weight;   /* unit weight, kg */
};

G_DEFINE_FINAL_TYPE(LoadItem, load_item, G_TYPE_OBJECT)

static guint changed_signal;

static void load_item_finalize(GObject *obj)
{
    LoadItem *self = LOAD_ITEM(obj);
    g_free(self->name);
    g_free(self->desc);
    G_OBJECT_CLASS(load_item_parent_class)->finalize(obj);
}

static void load_item_class_init(LoadItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = load_item_finalize;
    changed_signal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void load_item_init(LoadItem *self) { (void)self; }

static LoadItem *load_item_new(const char *name, const char *desc,
                               int qty, double weight)
{
    LoadItem *it = g_object_new(LOAD_TYPE_ITEM, NULL);
    it->name   = g_strdup(name);
    it->desc   = g_strdup(desc);
    it->qty    = qty;
    it->weight = weight;
    return it;
}

/* Returns a newly allocated string for the given column. */
static char *field_text(LoadItem *it, Field f)
{
    switch (f) {
    case F_NAME:   return g_strdup(it->name);
    case F_DESC:   return g_strdup(it->desc);
    case F_QTY:    return g_strdup_printf("%d", it->qty);
    case F_WEIGHT: return g_strdup_printf("%.1f", it->weight);
    case F_TOTAL:  return g_strdup_printf("%.1f", it->qty * it->weight);
    }
    return g_strdup("");
}

/* Parses user text into the item. Returns FALSE if the text is invalid. */
static gboolean load_item_apply(LoadItem *it, Field f, const char *text)
{
    char *end = NULL;

    switch (f) {
    case F_NAME:
        g_free(it->name);
        it->name = g_strdup(text);
        break;
    case F_DESC:
        g_free(it->desc);
        it->desc = g_strdup(text);
        break;
    case F_QTY: {
        gint64 v = g_ascii_strtoll(text, &end, 10);
        if (end == text || *end != '\0' || v < 0 || v > G_MAXINT)
            return FALSE;
        it->qty = (int)v;
        break;
    }
    case F_WEIGHT: {
        /* accept "12,5" as well as "12.5" */
        char *tmp = g_strdelimit(g_strdup(text), ",", '.');
        double v = g_ascii_strtod(tmp, &end);
        gboolean ok = (end != tmp && *end == '\0' && v >= 0);
        if (ok)
            it->weight = v;
        g_free(tmp);
        if (!ok)
            return FALSE;
        break;
    }
    default:
        return FALSE;
    }

    g_signal_emit(it, changed_signal, 0);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Application state                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    GListStore         *store;
    GtkSingleSelection *selection;
    GtkWidget          *total_label;
} App;

static App app;

static void update_total(void)
{
    double total = 0;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app.store));

    for (guint i = 0; i < n; i++) {
        LoadItem *it = g_list_model_get_item(G_LIST_MODEL(app.store), i);
        total += it->qty * it->weight;
        g_object_unref(it);
    }

    char *markup = g_markup_printf_escaped("<b>Total load: %.1f kg</b>", total);
    gtk_label_set_markup(GTK_LABEL(app.total_label), markup);
    g_free(markup);
}

static void on_item_changed(LoadItem *item, gpointer user_data)
{
    (void)item; (void)user_data;
    update_total();
}

static void add_item(LoadItem *item)   /* takes ownership */
{
    g_list_store_append(app.store, item);
    g_signal_connect(item, "changed", G_CALLBACK(on_item_changed), NULL);
    g_object_unref(item);
    update_total();
}

static void on_add_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    add_item(load_item_new("New item", "", 1, 0.0));
}

static void on_delete_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    guint pos = gtk_single_selection_get_selected(app.selection);
    if (pos != GTK_INVALID_LIST_POSITION) {
        g_list_store_remove(app.store, pos);
        update_total();
    }
}

/* ------------------------------------------------------------------ */
/* Column factories                                                    */
/* ------------------------------------------------------------------ */

static void on_editing_changed(GObject *widget, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;

    /* Only act when editing has just finished */
    if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(widget)))
        return;

    LoadItem *item = g_object_get_data(widget, "item");
    if (!item)
        return;

    Field field = GPOINTER_TO_INT(g_object_get_data(widget, "field"));
    const char *text = gtk_editable_get_text(GTK_EDITABLE(widget));

    load_item_apply(item, field, text);   /* ignores invalid input */

    /* Show the normalised value (or revert if the input was invalid) */
    char *shown = field_text(item, field);
    gtk_editable_set_text(GTK_EDITABLE(widget), shown);
    g_free(shown);
}

static void on_total_changed(LoadItem *item, gpointer label)
{
    char *t = field_text(item, F_TOTAL);
    gtk_label_set_text(GTK_LABEL(label), t);
    g_free(t);
}

static void setup_cb(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    (void)f;
    Field field = GPOINTER_TO_INT(data);
    GtkWidget *w;

    if (field == F_TOTAL) {
        w = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(w), 0.0);
    } else {
        w = gtk_editable_label_new("");
        g_object_set_data(G_OBJECT(w), "field", GINT_TO_POINTER(field));
        g_signal_connect(w, "notify::editing", G_CALLBACK(on_editing_changed), NULL);
    }
    gtk_widget_set_margin_start(w, 6);
    gtk_widget_set_margin_end(w, 6);
    gtk_list_item_set_child(li, w);
}

static void bind_cb(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    (void)f;
    Field field = GPOINTER_TO_INT(data);
    LoadItem *item = gtk_list_item_get_item(li);
    GtkWidget *w = gtk_list_item_get_child(li);
    char *text = field_text(item, field);

    if (field == F_TOTAL) {
        gtk_label_set_text(GTK_LABEL(w), text);
        gulong id = g_signal_connect(item, "changed",
                                     G_CALLBACK(on_total_changed), w);
        g_object_set_data(G_OBJECT(li), "handler-id", GSIZE_TO_POINTER(id));
    } else {
        gtk_editable_set_text(GTK_EDITABLE(w), text);
        g_object_set_data(G_OBJECT(w), "item", item);
    }
    g_free(text);
}

static void unbind_cb(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    (void)f;
    Field field = GPOINTER_TO_INT(data);
    GtkWidget *w = gtk_list_item_get_child(li);

    if (field == F_TOTAL) {
        gulong id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(li), "handler-id"));
        if (id)
            g_signal_handler_disconnect(gtk_list_item_get_item(li), id);
    } else {
        g_object_set_data(G_OBJECT(w), "item", NULL);
    }
}

static void add_column(GtkColumnView *view, const char *title, Field field)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup",  G_CALLBACK(setup_cb),  GINT_TO_POINTER(field));
    g_signal_connect(factory, "bind",   G_CALLBACK(bind_cb),   GINT_TO_POINTER(field));
    g_signal_connect(factory, "unbind", G_CALLBACK(unbind_cb), GINT_TO_POINTER(field));

    /* column takes ownership of the factory */
    GtkColumnViewColumn *col = gtk_column_view_column_new(title, factory);
    gtk_column_view_column_set_resizable(col, TRUE);
    if (field == F_NAME || field == F_DESC)
        gtk_column_view_column_set_expand(col, TRUE);
    gtk_column_view_append_column(view, col);
    g_object_unref(col);
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void activate(GtkApplication *gtkapp, gpointer data)
{
    (void)data;

    app.store = g_list_store_new(LOAD_TYPE_ITEM);
    app.selection = gtk_single_selection_new(g_object_ref(G_LIST_MODEL(app.store)));

    GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(app.selection));
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(view), TRUE);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(view), TRUE);

    add_column(GTK_COLUMN_VIEW(view), "Item",          F_NAME);
    add_column(GTK_COLUMN_VIEW(view), "Description",   F_DESC);
    add_column(GTK_COLUMN_VIEW(view), "Qty",           F_QTY);
    add_column(GTK_COLUMN_VIEW(view), "Unit wt (kg)",  F_WEIGHT);
    add_column(GTK_COLUMN_VIEW(view), "Total wt (kg)", F_TOTAL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);

    GtkWidget *add_btn = gtk_button_new_with_label("Add row");
    GtkWidget *del_btn = gtk_button_new_with_label("Delete selected");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), NULL);
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_clicked), NULL);

    app.total_label = gtk_label_new(NULL);
    gtk_widget_set_hexpand(app.total_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(app.total_label), 1.0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(bar), add_btn);
    gtk_box_append(GTK_BOX(bar), del_btn);
    gtk_box_append(GTK_BOX(bar), app.total_label);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), bar);

    GtkWidget *win = gtk_application_window_new(gtkapp);
    gtk_window_set_title(GTK_WINDOW(win), "Load List");
    gtk_window_set_default_size(GTK_WINDOW(win), 750, 400);
    gtk_window_set_child(GTK_WINDOW(win), box);

    add_item(load_item_new("Pump",  "Centrifugal pump", 2,  45.5));
    add_item(load_item_new("Valve", "Gate valve DN50",  10, 6.2));
    add_item(load_item_new("Cable", "Cable drum",       3,  120.0));

    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv)
{
    GtkApplication *gtkapp =
        gtk_application_new("org.example.LoadList", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtkapp, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(gtkapp), argc, argv);
    g_object_unref(gtkapp);
    return status;
}
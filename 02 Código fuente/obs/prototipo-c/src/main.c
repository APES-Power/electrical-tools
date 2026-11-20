
#include <gtk/gtk.h>

typedef struct {
    GPtrArray *tags;
    gchar *csv_path;
    GtkWidget *tag_label;
} app_data_t;


/* --------------------------------------------------
 * CSV
 * -------------------------------------------------- */

static gboolean parse_csv_line(const gchar *line,
                               gchar **name,
                               gchar **tag)
{
    const gchar *p = line;
    GString *fields[2];

    for (gint i = 0; i < 2; i++) {
        fields[i] = g_string_new(NULL);

        while (g_ascii_isspace(*p) && *p != '\0')
            p++;

        if (*p == '"') {
            p++;

            while (*p != '\0') {
                if (*p == '"') {
                    if (*(p + 1) == '"') {
                        g_string_append_c(fields[i], '"');
                        p += 2;
                    } else {
                        p++;
                        break;
                    }
                } else {
                    g_string_append_c(fields[i], *p);
                    p++;
                }
            }
        } else {
            while (*p != '\0' && *p != ',') {
                g_string_append_c(fields[i], *p);
                p++;
            }
        }

        if (i == 0) {
            while (g_ascii_isspace(*p) && *p != '\0')
                p++;

            if (*p != ',') {
                g_string_free(fields[0], TRUE);
                g_string_free(fields[1], TRUE);
                return FALSE;
            }

            p++;
        }
    }

    while (g_ascii_isspace(*p) && *p != '\0')
        p++;

    if (*p != '\0') {
        g_string_free(fields[0], TRUE);
        g_string_free(fields[1], TRUE);
        return FALSE;
    }

    *name = g_strdup(g_strstrip(g_string_free(fields[0], FALSE)));
    *tag  = g_strdup(g_strstrip(g_string_free(fields[1], FALSE)));

    if (**name == '\0' || **tag == '\0') {
        g_free(*name);
        g_free(*tag);
        *name = NULL;
        *tag = NULL;
        return FALSE;
    }

    return TRUE;
}


/* --------------------------------------------------
 * CARGAR TIPOS DESDE CSV
 * -------------------------------------------------- */

static gboolean load_types_from_csv(const gchar *filename,
                                    GtkStringList *names,
                                    GPtrArray *tags)
{
    gchar *contents = NULL;
    GError *error = NULL;

    if (!g_file_get_contents(filename, &contents, NULL, &error)) {
        g_warning("Error leyendo CSV: %s", error->message);
        g_error_free(error);
        return FALSE;
    }

    gchar **lines = g_strsplit(contents, "\n", -1);

    for (guint i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strdup(lines[i]);
        g_strchomp(line);

        if (*line == '\0') {
            g_free(line);
            continue;
        }

        /* Saltar cabecera */
        if (i == 0 && g_ascii_strncasecmp(line, "descripcion,", 12) == 0) {
            g_free(line);
            continue;
        }

        gchar *name = NULL;
        gchar *tag = NULL;

        if (parse_csv_line(line, &name, &tag)) {
            gtk_string_list_append(names, name);
            g_ptr_array_add(tags, tag);
            g_free(name);
        } else {
            g_warning("Linea CSV no valida: %s", line);
        }

        g_free(line);
    }

    g_strfreev(lines);
    g_free(contents);

    return tags->len > 0;
}


/* --------------------------------------------------
 * ACTUALIZAR TAG SELECCIONADO
 * -------------------------------------------------- */

static void on_type_selected(GObject *object,
                             GParamSpec *pspec,
                             gpointer user_data)
{
    app_data_t *data = user_data;

    guint selected = gtk_drop_down_get_selected(
        GTK_DROP_DOWN(object)
    );

    if (selected == GTK_INVALID_LIST_POSITION ||
        selected >= data->tags->len)
        return;

    const gchar *tag = g_ptr_array_index(data->tags, selected);

    gchar *text = g_strdup_printf("Tag seleccionado: %s", tag);

    gtk_label_set_text(GTK_LABEL(data->tag_label), text);

    g_free(text);
}


/* --------------------------------------------------
 * INTERFAZ GTK
 * -------------------------------------------------- */

static void activate(GtkApplication *app,
                     gpointer user_data)
{
    app_data_t *data = user_data;

    GtkStringList *names = gtk_string_list_new(NULL);

    data->tags = g_ptr_array_new_with_free_func(g_free);

    if (!load_types_from_csv(data->csv_path, names, data->tags)) {
        GtkWidget *error_window = gtk_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(error_window), "Error");
        gtk_window_set_default_size(GTK_WINDOW(error_window), 350, 100);

        GtkWidget *label = gtk_label_new(
            "No se han podido cargar los tipos de carga."
        );

        gtk_window_set_child(GTK_WINDOW(error_window), label);
        gtk_window_present(GTK_WINDOW(error_window));

        g_object_unref(names);
        return;
    }

    GtkWidget *window = gtk_application_window_new(app);

    gtk_window_set_title(GTK_WINDOW(window), "Tipos de carga");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 150);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);

    GtkWidget *title = gtk_label_new("Seleccionar tipo de carga");

    GtkWidget *dropdown = gtk_drop_down_new(
        G_LIST_MODEL(names),
        NULL
    );

    data->tag_label = gtk_label_new("Tag seleccionado:");

    g_signal_connect(
        dropdown,
        "notify::selected",
        G_CALLBACK(on_type_selected),
        data
    );

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), dropdown);
    gtk_box_append(GTK_BOX(box), data->tag_label);

    gtk_window_set_child(GTK_WINDOW(window), box);

    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), 0);

    gtk_window_present(GTK_WINDOW(window));
}


/* --------------------------------------------------
 * MAIN
 * -------------------------------------------------- */

int main(int argc, char **argv)
{
    app_data_t data = {
        .tags = NULL,
        .csv_path = argc > 1 ? g_strdup(argv[1])
                             : g_strdup("load_types.csv"),
        .tag_label = NULL
    };

    GtkApplication *app = gtk_application_new(
        "com.example.loadtypes",
        G_APPLICATION_DEFAULT_FLAGS
    );

    g_signal_connect(
        app,
        "activate",
        G_CALLBACK(activate),
        &data
    );

    int status = g_application_run(
        G_APPLICATION(app),
        1,
        argv
    );

    if (data.tags != NULL)
        g_ptr_array_unref(data.tags);

    g_free(data.csv_path);
    g_object_unref(app);

    return status;
}

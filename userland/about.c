#include <gtk/gtk.h>
#include <stdio.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>

/*
 * AscentOS "About" Application
 * A premium, modern system information utility.
 */

typedef struct {
  GtkWidget *window;
  GtkWidget *uptime_label;
  GtkWidget *mem_label;
  GtkWidget *mem_progress;
} AppState;

static gboolean update_info(gpointer data) {
  AppState *state = (AppState *)data;
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    // Update Uptime
    int secs = info.uptime;
    int mins = secs / 60;
    int hrs = mins / 60;
    int days = hrs / 24;

    char uptime_buf[128];
    if (days > 0) {
      snprintf(uptime_buf, sizeof(uptime_buf), "%d days, %02d:%02d:%02d", days,
               hrs % 24, mins % 60, secs % 60);
    } else {
      snprintf(uptime_buf, sizeof(uptime_buf), "%02d:%02d:%02d", hrs, mins % 60,
               secs % 60);
    }
    gtk_label_set_text(GTK_LABEL(state->uptime_label), uptime_buf);

    // Update Memory
    double total_gb = info.totalram / (1024.0 * 1024.0 * 1024.0);
    double free_gb = info.freeram / (1024.0 * 1024.0 * 1024.0);
    double used_gb = total_gb - free_gb;

    char mem_buf[128];
    snprintf(mem_buf, sizeof(mem_buf), "%.2f GB / %.2f GB", used_gb, total_gb);
    gtk_label_set_text(GTK_LABEL(state->mem_label), mem_buf);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->mem_progress),
                                  used_gb / total_gb);
  }
  return TRUE;
}

static gboolean on_draw_logo(GtkWidget *widget, cairo_t *cr, gpointer data) {
  int w = gtk_widget_get_allocated_width(widget);
  int h = gtk_widget_get_allocated_height(widget);

  // Draw a stylized "A" for AscentOS
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

  // Background glow
  cairo_pattern_t *pat =
      cairo_pattern_create_radial(w / 2, h / 2, 5, w / 2, h / 2, w / 2);
  cairo_pattern_add_color_stop_rgba(pat, 0, 0.2, 0.6, 1.0, 0.3);
  cairo_pattern_add_color_stop_rgba(pat, 1, 0.2, 0.6, 1.0, 0.0);
  cairo_set_source(cr, pat);
  cairo_paint(cr);
  cairo_pattern_destroy(pat);

  // Rocket / Arrow pointing up
  cairo_set_source_rgb(cr, 0.3, 0.7, 1.0);
  cairo_set_line_width(cr, 6);

  cairo_move_to(cr, w / 2, h / 4);
  cairo_line_to(cr, w / 4, 3 * h / 4);
  cairo_line_to(cr, w / 2, 5 * h / 8);
  cairo_line_to(cr, 3 * w / 4, 3 * h / 4);
  cairo_close_path(cr);
  cairo_stroke_preserve(cr);

  cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.2);
  cairo_fill(cr);

  return FALSE;
}

int main(int argc, char *argv[]) {
  AppState state;
  gtk_init(&argc, &argv);

  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "About AscentOS");
  gtk_window_set_default_size(GTK_WINDOW(window), 450, 600);
  gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(window), scrolled);

  GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(scrolled), main_vbox);

  // Header Color Strip
  GtkWidget *header_strip = gtk_drawing_area_new();
  gtk_widget_set_size_request(header_strip, -1, 5);
  gtk_box_pack_start(GTK_BOX(main_vbox), header_strip, FALSE, FALSE, 0);
  g_signal_connect(header_strip, "draw", G_CALLBACK(NULL),
                   NULL); // Just a spacer or can be painted

  // Logo Area
  GError *error = NULL;
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale("/assets/logo.png", 128,
                                                        128, TRUE, &error);
  if (pixbuf) {
    GtkWidget *logo_image = gtk_image_new_from_pixbuf(pixbuf);
    gtk_box_pack_start(GTK_BOX(main_vbox), logo_image, FALSE, FALSE, 20);
    g_object_unref(pixbuf);
  } else {
    GtkWidget *logo_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(logo_area, -1, 100);
    g_signal_connect(logo_area, "draw", G_CALLBACK(on_draw_logo), NULL);
    gtk_box_pack_start(GTK_BOX(main_vbox), logo_area, FALSE, FALSE, 20);
    if (error)
      g_error_free(error);
  }

  // Title
  GtkWidget *title_label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(title_label),
                       "<span size='xx-large' weight='bold'>Ascent</span><span "
                       "size='xx-large'>OS</span>");
  gtk_box_pack_start(GTK_BOX(main_vbox), title_label, FALSE, FALSE, 0);

  GtkWidget *version_label = gtk_label_new("Version 2.0.0-beta");
  gtk_widget_set_opacity(version_label, 0.6);
  gtk_box_pack_start(GTK_BOX(main_vbox), version_label, FALSE, FALSE, 5);

  // Separator
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start(GTK_BOX(main_vbox), sep, FALSE, FALSE, 20);

  // Info Grid
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 15);
  gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(main_vbox), grid, FALSE, FALSE, 10);

  int row = 0;

  // Kernel Info
  struct utsname un;
  uname(&un);

  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Kernel:"), 0, row, 1, 1);
  GtkWidget *kernel_val = gtk_label_new(un.release);
  gtk_widget_set_halign(kernel_val, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), kernel_val, 1, row++, 1, 1);

  // Architecture
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Architecture:"), 0, row, 1, 1);
  GtkWidget *arch_val = gtk_label_new(un.machine);
  gtk_widget_set_halign(arch_val, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), arch_val, 1, row++, 1, 1);

  // Uptime
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Uptime:"), 0, row, 1, 1);
  state.uptime_label = gtk_label_new("Calculating...");
  gtk_widget_set_halign(state.uptime_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), state.uptime_label, 1, row++, 1, 1);

  // Memory
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Memory:"), 0, row, 1, 1);
  GtkWidget *mem_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  state.mem_label = gtk_label_new("Calculating...");
  gtk_widget_set_halign(state.mem_label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(mem_vbox), state.mem_label, FALSE, FALSE, 0);
  state.mem_progress = gtk_progress_bar_new();
  gtk_widget_set_size_request(state.mem_progress, 150, -1);
  gtk_box_pack_start(GTK_BOX(mem_vbox), state.mem_progress, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), mem_vbox, 1, row++, 1, 1);

  // Footer Credits
  GtkWidget *footer_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start(GTK_BOX(main_vbox), footer_sep, FALSE, FALSE, 20);

  GtkWidget *credits_label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(credits_label),
                       "<span size='small'>Inspired by modern OS design\nBuilt "
                       "for the <b>AscentOS</b> community</span>");
  gtk_label_set_justify(GTK_LABEL(credits_label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_opacity(credits_label, 0.7);
  gtk_box_pack_start(GTK_BOX(main_vbox), credits_label, FALSE, FALSE, 10);

  GtkWidget *desc_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(desc_label), 
        "<span size='medium' style='italic'>\"AscentOS is a hobby operating system "
        "that dedicates to become a daily usable OS for everyone \"</span>");
    gtk_label_set_line_wrap(GTK_LABEL(desc_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(desc_label), 50);
    gtk_label_set_justify(GTK_LABEL(desc_label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(main_vbox), desc_label, FALSE, FALSE, 10);

    // Close Button
    GtkWidget *btn_box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(btn_box), GTK_BUTTONBOX_CENTER);
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(close_btn, 100, -1);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(gtk_main_quit), NULL);
    gtk_container_add(GTK_CONTAINER(btn_box), close_btn);
    gtk_box_pack_end(GTK_BOX(main_vbox), btn_box, FALSE, FALSE, 20);

    // Start Timer
    g_timeout_add(1000, update_info, &state);
    update_info(&state);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}

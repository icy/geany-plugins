/*
 *      bptree.c
 *      
 *      Copyright 2010 Alexander Petukhov <devel(at)apetukhov.ru>
 *      
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *      
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *      
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 */

/*
 * 		Contains breakpoints GtkTreeView manipulating functions 
 * 		and handlers for tree view events.
 * 		Handlers only collect data from the tree and call
 * 		corresponding breaks_set_.... functions, which call
 * 		bptree_set... if breakpoint has been changed altered/added/removed
 */

#include <stdlib.h>
#include <memory.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#ifdef HAVE_CONFIG_H
	#include "config.h"
#endif
#include <geanyplugin.h>

#include "breakpoints.h"
#include "bptree.h"
#include "utils.h"
#include "dconfig.h"
#include "tabs.h"
#include "pixbuf.h"

/* Tree view columns */
enum
{
   ICON,
   FILEPATH,
   CONDITION,
   HITSCOUNT,
   LINE,
   ENABLED,
   LAST_VISIBLE,
   N_COLUMNS
};

/* tree view and store handles */
static GtkWidget		*tree = NULL;
static GtkTreeModel		*model = NULL;
static GtkTreeStore		*store = NULL;

/* column cell renderes */
static GtkCellRenderer	*enable_renderer;
static GtkCellRenderer	*hcount_renderer;
static GtkCellRenderer	*condition_renderer;

/* tells to checkbox click handler whether page is in readonly mode (debug running) */
static gboolean readonly = FALSE;

/* hash table to keep file nodes in the tree */
static GHashTable *files;

/* callback handler */
move_to_line_cb on_break_clicked = NULL;

/* 
 * gets tree row reference for an unsected row at the same depth
 */
GtkTreeRowReference* get_unselected_sibling(GtkTreePath *path)
{
	GtkTreeRowReference *sibling = NULL;
	
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));

	/* move down find first unselected sibling */
	GtkTreeIter titer;
	gtk_tree_model_get_iter(model, &titer, path);
	while (gtk_tree_model_iter_next(model, &titer))
	{
		if (!gtk_tree_selection_iter_is_selected(selection, &titer))
		{
			GtkTreePath *sibling_path = gtk_tree_model_get_path(model, &titer);
			sibling = gtk_tree_row_reference_new(model, sibling_path);
			gtk_tree_path_free(sibling_path);
			break;
		}
	}

	if (!sibling)
	{
		/* move up find first unselected sibling */
		GtkTreePath *sibling_path = gtk_tree_path_copy(path);
		while (gtk_tree_path_prev(sibling_path))
		{
			if (!gtk_tree_selection_path_is_selected(selection, sibling_path))
			{
				sibling = gtk_tree_row_reference_new(model, sibling_path);
				break;
			}
		}
		gtk_tree_path_free(sibling_path);
	}

	return sibling;
}

/* 
 * checks file ENABLED column if all childs are enabled and unchecks otherwise
 */
static void update_file_node(GtkTreeIter *file_iter)
{
	GtkTreeIter child;
	gboolean check = TRUE;
	if(gtk_tree_model_iter_children(model, &child, file_iter))
	{
		do
		{
			gboolean enabled;
			gtk_tree_model_get (
				model,
				&child,
				ENABLED, &enabled,
				-1);
				
			if (!enabled)
			{
				check = FALSE;
				break;
			}
		}
		while(gtk_tree_model_iter_next(model, &child));
	}

	gtk_tree_store_set(store, file_iter, ENABLED, check, -1);
}

/* 
 * GtkTreeView cell renderer functions that do not render and make ineditable file rows
 */
void on_render(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell, GtkTreeModel *tree_model,
	GtkTreeIter *iter, gpointer data)
{
	GtkTreePath *path = gtk_tree_model_get_path(model, iter);
	if (data)
	{
		if (gtk_tree_path_get_depth(path) == 1)
		{
			g_object_set(cell, "text", "", NULL);
			g_object_set(cell, "editable", FALSE, NULL);
		}
		else
		{
			g_object_set(cell, "editable", TRUE, NULL);
		}
	}
	else
	{
		if (gtk_tree_path_get_depth(path) == 1)
		{
			g_object_set(cell, "visible", FALSE, NULL);
		}
		else
		{
			g_object_set(cell, "visible", TRUE, NULL);
		}
	}
	gtk_tree_path_free(path);
}

/* 
 * GtkTreeView event handlers
 */

/*
 * double click
 */
static void on_row_double_click(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data)
{
	if (1 == gtk_tree_path_get_depth(path))
	{
		return;
	}
	
	GtkTreeIter iter;
	gtk_tree_model_get_iter (
		 model,
		 &iter,
		 path);
    
    gchar *file;
	int line;
	
	GtkTreeIter parent_iter;
	gtk_tree_model_iter_parent(model, &parent_iter, &iter);
	
	gtk_tree_model_get (
		model,
		&parent_iter,
		FILEPATH, &file,
        -1);

	gtk_tree_model_get (
		model,
		&iter,
		LINE, &line,
        -1);
        
    /* use callback, supplied in bptree_init */
	on_break_clicked(file, line);
	
	g_free(file);
}

/*
 * editing "condition" column value finished
 */
void on_condition_changed(GtkCellRendererText *renderer, gchar *path, gchar *new_text, gpointer user_data)
{
	GtkTreeIter  iter;
    GtkTreePath *tree_path = gtk_tree_path_new_from_string (path);

	gtk_tree_model_get_iter (
		 model,
		 &iter,
		 tree_path);
	
	GtkTreeIter parent_iter;
	gtk_tree_model_iter_parent(model, &parent_iter, &iter);

	gchar *file;
	gtk_tree_model_get (
		model,
		&parent_iter,
		FILEPATH, &file,
		-1);
	
	int line;
	gchar* oldcondition;
	gtk_tree_model_get (
		model,
		&iter,
		CONDITION, &oldcondition,
		LINE, &line,
        -1);
        
    if (strcmp(oldcondition, new_text))
		breaks_set_condition(file, line, new_text);
	
	gtk_tree_path_free(tree_path);
	g_free(file);
	g_free(oldcondition);
}

/*
 * editing "hitscount" column value finished
 */
void on_hitscount_changed(GtkCellRendererText *renderer, gchar *path, gchar *new_text, gpointer user_data)
{
	int count = atoi(new_text);
	if (!count && strcmp(new_text, "0"))
		return;

	GtkTreeIter  iter;
    GtkTreePath *tree_path = gtk_tree_path_new_from_string (path);

	gtk_tree_model_get_iter (
		 model,
		 &iter,
		 tree_path);

	GtkTreeIter parent_iter;
	gtk_tree_model_iter_parent(model, &parent_iter, &iter);

	gchar *file;
	gtk_tree_model_get (
		model,
		&parent_iter,
		FILEPATH, &file,
		-1);

	int line;
	gint oldcount;
	gtk_tree_model_get (
		model,
		&iter,
		HITSCOUNT, &oldcount,
		LINE, &line,
        -1);
        
    if (oldcount != count)
    	breaks_set_hits_count(file, line, count);
	
	gtk_tree_path_free(tree_path);
	g_free(file);
}                                                        

/*
 * "Enabled" checkbox has been clicked
 */
void on_activeness_changed(GtkCellRendererToggle *cell_renderer, gchar *path, gpointer user_data)
{
	/* do not process event is page is readonly (debug is running) */
	if (readonly)
		return;
	
	GtkTreeIter  iter;
    GtkTreePath *tree_path = gtk_tree_path_new_from_string (path);
	
	gtk_tree_model_get_iter (
		model,
		&iter,
		tree_path);
    
	gboolean current_state = gtk_cell_renderer_toggle_get_active(cell_renderer);

    /* check if this is a file row */
    if(1 == gtk_tree_path_get_depth(tree_path))
    {
		gchar *file;
		gtk_tree_model_get (
			model,
			&iter,
			FILEPATH, &file,
			-1);

		breaks_set_enabled_for_file(file, !current_state);

		g_free(file);
	}
	else
	{
		GtkTreeIter parent_iter;
		gtk_tree_model_iter_parent(model, &parent_iter, &iter);

		gchar *file;
		int line;

		gtk_tree_model_get (
			model,
			&parent_iter,
			FILEPATH, &file,
			-1);
		gtk_tree_model_get (
			model,
			&iter,
			LINE, &line,
			-1);
			
		breaks_switch(file, line);
		
		g_free(file);
	}

	gtk_tree_path_free(tree_path);
}

/*
 * key pressed event
 */
static gboolean on_key_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	/* do not process event is page is readonly (debug is running) */
	if (readonly)
		return FALSE;

    guint keyval = ((GdkEventKey*)event)->keyval;

	/* get selected rows */
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
	GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
	rows = g_list_sort(rows, (GCompareFunc)gtk_tree_path_compare);

	if (keyval == GDK_Delete && rows && g_list_length(rows))
	{
		/* "delete selected rows" */

		/* get new selection */
		GtkTreeRowReference *new_selection = NULL;
		GtkTreePath *first_path = (GtkTreePath*)rows->data;
		if (gtk_tree_path_get_depth(first_path) > 1)
		{
			new_selection = get_unselected_sibling(first_path);
		}
		if (!new_selection)
		{
			GtkTreePath *file_path = gtk_tree_path_copy(first_path);
			if (gtk_tree_path_get_depth(file_path) > 1)
			{
				gtk_tree_path_up(file_path);
			}
			new_selection = get_unselected_sibling(file_path);
			gtk_tree_path_free(file_path);
		}
		
		/* collect GList of breakpoints to remove
		if file row is met - add all unselected breaks to the list as well */
		GList *iter = rows;
		GList *breaks = NULL;
		while (iter)
		{
			GtkTreePath *path = (GtkTreePath*)iter->data;
			
			GtkTreeIter titer;
			gtk_tree_model_get_iter(model, &titer, path);
			
			if (1 == gtk_tree_path_get_depth(path))
			{
				GtkTreeIter citer;
				gtk_tree_model_iter_children(model, &citer, &titer);
				
				do
				{
					if (!gtk_tree_selection_iter_is_selected(selection, &citer))
					{
						gchar *file = NULL;
						gint line;

						gtk_tree_model_get(model, &titer, FILEPATH, &file, -1);
						gtk_tree_model_get(model, &citer, LINE, &line, -1);

						breakpoint *bp = breaks_lookup_breakpoint(file, line);
						
						breaks = g_list_append(breaks, bp);
						
						g_free(file);
					}
				}
				while(gtk_tree_model_iter_next(model, &citer));
			}
			else
			{
				GtkTreeIter piter;
				gtk_tree_model_iter_parent(model, &piter, &titer);
				
				gchar *file = NULL;
				gtk_tree_model_get(model, &piter, FILEPATH, &file, -1);

				gint line;
				gtk_tree_model_get(model, &titer, LINE, &line, -1);

				breakpoint *bp = breaks_lookup_breakpoint(file, line);
				
				breaks = g_list_append(breaks, bp);
				
				g_free(file);
			}
			iter = iter->next;
		}
		
		if (1 == g_list_length(breaks))
		{
			breakpoint *bp = (breakpoint*)breaks->data;
			g_list_free(breaks);
			breaks_remove(bp->file, bp->line);
		}
		else
		{
			breaks_remove_list(breaks);
		}

		if (new_selection)
		{
			/* get path to select */
			GtkTreePath *path = NULL;
			path = gtk_tree_row_reference_get_path(new_selection);

			gtk_tree_selection_select_path(selection, path);
			gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(widget), path, NULL, TRUE, 0.5, 0.5);
			gtk_tree_path_free(path);

			gtk_tree_row_reference_free(new_selection);
		}
	}

	/* free rows list */
	g_list_foreach (rows, (GFunc)gtk_tree_path_free, NULL);
	g_list_free (rows);

	return FALSE;
}

/*
 * update existing breakpoint icon
 * arguments:
 * 		bp - breakpoint to update
 */
void bptree_update_break_icon(breakpoint* bp)
{
	GdkPixbuf *pixbuf;
	if (!bp->enabled)
	{
		pixbuf = break_disabled_pixbuf;
	}
	else if (bp->hitscount || strlen(bp->condition))
	{
		pixbuf = break_condition_pixbuf;
	}
	else
	{
		pixbuf = break_pixbuf;
	}

	gtk_tree_store_set (store, &bp->iter, ICON, pixbuf, -1);
}

/*
 * Interface functions
 */

/*
 * init breaks tree view and return it if succesfull
 * arguments:
 * 		cb - callback to call on treeview double click
 */
gboolean bptree_init(move_to_line_cb cb)
{
	/* save double click callback */
	on_break_clicked = cb;
	
	/* crete hash table for file nodes */
	files = g_hash_table_new_full(
		g_str_hash,
		g_str_equal,
		(GDestroyNotify)g_free,
		(GDestroyNotify)gtk_tree_row_reference_free
	);
	
	/* create tree view */
	store = gtk_tree_store_new (
		N_COLUMNS,
		GDK_TYPE_PIXBUF,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_INT,
		G_TYPE_INT,
		G_TYPE_BOOLEAN,
		G_TYPE_STRING);
	model = GTK_TREE_MODEL(store);
	tree = gtk_tree_view_new_with_model (model);
	
	/* set tree view properties */
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), 1);
	/* multiple selection */
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
	
	/* connect signals */
	g_signal_connect(G_OBJECT(tree), "key-press-event", G_CALLBACK (on_key_pressed), NULL);
	g_signal_connect(G_OBJECT(tree), "row-activated", G_CALLBACK (on_row_double_click), NULL);

	/* creating columns */
	GtkTreeViewColumn	*column;

	/* icon, file */
	GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new();
	gtk_tree_view_column_pack_end(column, renderer, TRUE);
	gtk_tree_view_column_set_attributes(column, renderer, "text", FILEPATH, NULL);	
	GtkCellRenderer *icon_renderer = gtk_cell_renderer_pixbuf_new ();
	g_object_set(icon_renderer, "follow-state", TRUE, NULL);
	gtk_tree_view_column_pack_end(column, icon_renderer, FALSE);
	gtk_tree_view_column_set_attributes(column, icon_renderer, "pixbuf", ICON, NULL);	
	gtk_tree_view_column_set_title(column, _("Location"));
	gtk_tree_view_column_set_cell_data_func(column, icon_renderer, on_render, NULL, NULL);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* condition */
	condition_renderer = gtk_cell_renderer_text_new ();
	g_object_set (condition_renderer, "editable", TRUE, NULL);
	g_object_set (condition_renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	g_signal_connect (G_OBJECT (condition_renderer), "edited", G_CALLBACK (on_condition_changed), NULL);
	column = gtk_tree_view_column_new_with_attributes (_("Condition"), condition_renderer, "text", CONDITION, NULL);
	gtk_tree_view_column_set_cell_data_func(column, condition_renderer, on_render, (gpointer)TRUE, NULL);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);
	
	/* hits count */
	hcount_renderer = gtk_cell_renderer_spin_new ();
	GtkAdjustment* adj = GTK_ADJUSTMENT (gtk_adjustment_new (0.0, 0.0, 100000.0, 1.0, 2.0, 2.0));
	g_object_set (hcount_renderer,
		"adjustment", adj,
        "digits", 0, NULL);
	g_signal_connect (G_OBJECT (hcount_renderer), "edited", G_CALLBACK (on_hitscount_changed), NULL);
	column = gtk_tree_view_column_new_with_attributes (_("Hit count"), hcount_renderer, "text", HITSCOUNT, NULL);
	gtk_tree_view_column_set_cell_data_func(column, hcount_renderer, on_render, (gpointer)TRUE, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);
	
	/* line */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Line"), renderer, "text", LINE, NULL);
	gtk_tree_view_column_set_visible(column, FALSE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* enabled */
	enable_renderer = gtk_cell_renderer_toggle_new ();
	g_signal_connect (G_OBJECT(enable_renderer), "toggled", G_CALLBACK(on_activeness_changed), NULL);
	column = gtk_tree_view_column_new_with_attributes (_("Enabled"), enable_renderer, "active", ENABLED, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* Last invisible column */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes ("", renderer, "text", LAST_VISIBLE, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	tab_breaks = gtk_scrolled_window_new (NULL, NULL);
	gtk_widget_show (tab_breaks);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (tab_breaks), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (tab_breaks), GTK_SHADOW_NONE);

	gtk_container_add (GTK_CONTAINER (tab_breaks), tree);

	return TRUE;
}

/*
 * destroy breaks tree and associated data
 * arguments:
 */
void bptree_destroy()
{
	g_hash_table_destroy(files);
}

/*
 * enable/disable break
 * arguments:
 * 		bp 	- breakpoint
 */
void bptree_set_enabled(breakpoint *bp)
{
	gtk_tree_store_set(store, &(bp->iter), ENABLED, bp->enabled, -1);
	bptree_update_break_icon(bp);

	GtkTreeIter parent;
	gtk_tree_model_iter_parent(model, &parent, &(bp->iter));
	update_file_node(&parent);
}

/*
 * set breaks hits count
 * arguments:
 * 		bp 	- breakpoint
 */
void bptree_set_hitscount(breakpoint *bp)
{
  gtk_tree_store_set(store, &(bp->iter), HITSCOUNT, bp->hitscount, -1);
  bptree_update_break_icon(bp);
}

/*
 * set breaks condition
 * arguments:
 * 		bp 	- breakpoint
 */
void bptree_set_condition(breakpoint* bp)
{
  gtk_tree_store_set(store, &(bp->iter), CONDITION, bp->condition, -1);
  bptree_update_break_icon(bp);
}

/*
 * get breaks condition
 * arguments:
 * 		iter 	- tree view iterator
 * return value	- breaks condition
 */
gchar* bptree_get_condition(breakpoint *bp)
{
    gchar *condition;
	gtk_tree_model_get (
		model,
		&(bp->iter),
		CONDITION, &condition,
        -1);

	return condition;
}

/*
 * set tree view accessible / inaccessible to user input 
 * arguments:
 * 		value - new value
 */
void bptree_set_readonly(gboolean value)
{
	readonly = value;
    g_object_set (hcount_renderer, "editable", !readonly, NULL);
    g_object_set (condition_renderer, "editable", !readonly, NULL);
}

/*
 * add new breakpoint to the tree view
 * arguments:
 * 		bp - breakpoint to add
 */
void bptree_add_breakpoint(breakpoint* bp)
{
	GtkTreeIter file_iter;
	GtkTreeRowReference *file_reference = (GtkTreeRowReference*)g_hash_table_lookup(files, bp->file);
	if (!file_reference)
	{
		gtk_tree_store_prepend (store, &file_iter, NULL);
		gtk_tree_store_set (store, &file_iter,
						FILEPATH, bp->file,
						ENABLED, TRUE,
						-1);

		GtkTreePath *file_path = gtk_tree_model_get_path(model, &file_iter);
		file_reference = gtk_tree_row_reference_new(model, file_path);
		gtk_tree_path_free(file_path);

		g_hash_table_insert(files, (gpointer)g_strdup(bp->file),(gpointer)file_reference);
	}
	else
	{
		gtk_tree_model_get_iter(model, &file_iter, gtk_tree_row_reference_get_path(file_reference));
	}
	
	/* lookup where to insert new row */
	GtkTreeIter child, *sibling = NULL;
	if(gtk_tree_model_iter_children(model, &child, &file_iter))
	{
		do
		{
			int line;
			gtk_tree_model_get (
				model,
				&child,
				LINE, &line,
				-1);
			if (line > bp->line)
			{
				sibling = &child;
				break;
			}
		}
		while(gtk_tree_model_iter_next(model, &child));
	}
	
	GtkTreeIter iter;
	gtk_tree_store_insert_before(store, &iter, &file_iter, sibling);
    bp->iter = iter;
    
    bptree_update_breakpoint(bp);
}

/*
 * update existing breakpoint
 * arguments:
 * 		bp - breakpoint to update
 */
void bptree_update_breakpoint(breakpoint* bp)
{
	gchar *location = g_strdup_printf(_("line %i"), bp->line);
	
	gtk_tree_store_set (store, &bp->iter,
                    ENABLED, bp->enabled,
                    HITSCOUNT, bp->hitscount,
                    CONDITION, bp->condition,
                    FILEPATH, location,
                    LINE, bp->line,
                    -1);
	
	bptree_update_break_icon(bp);

    g_free(location);
}

/*
 * remove breakpoint
 * arguments:
 * 		bp - breakpoint to revove
 */
void bptree_remove_breakpoint(breakpoint* bp)
{
	GtkTreeIter file;
	gtk_tree_model_iter_parent(model, &file, &(bp->iter));
	
	gtk_tree_store_remove(store, &(bp->iter));

	if (!gtk_tree_model_iter_n_children(model, &file))
	{
		g_hash_table_remove(files, (gpointer)bp->file);
		gtk_tree_store_remove(store, &file);
	}
	else
	{
		update_file_node(&file);
	}
}

/*
 * updates all file ENABLED checkboxes base on theit children states
 * arguments:
 */
void bptree_update_file_nodes()
{
	GtkTreeIter file;
	if(gtk_tree_model_iter_children(model, &file, NULL))
	{
		do
		{
			update_file_node(&file);
		}
		while(gtk_tree_model_iter_next(model, &file));
	}
}

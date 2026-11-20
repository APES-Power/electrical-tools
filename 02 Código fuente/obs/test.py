#!/usr/bin/env python3
"""Load list table using GTK4: Gtk.ColumnView + Gio.ListStore.
Qty is editable (click the cell, type, press Enter); the total updates live.
"""
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gio, GObject


class Item(GObject.Object):
    name = GObject.Property(type=str)
    desc = GObject.Property(type=str)
    qty = GObject.Property(type=int, default=1)
    weight = GObject.Property(type=float)

    def __init__(self, name, desc, qty, weight):
        super().__init__(name=name, desc=desc, qty=qty, weight=weight)


def text_column(title, getter, editable_setter=None):
    """Build a ColumnViewColumn. getter(item)->str; setter(item, str) makes it editable."""
    factory = Gtk.SignalListItemFactory()

    def setup(_f, list_item):
        if editable_setter:
            w = Gtk.EditableLabel()
        else:
            w = Gtk.Label(xalign=0)
        list_item.set_child(w)

    def bind(_f, list_item):
        item, w = list_item.get_item(), list_item.get_child()
        w.set_text(getter(item))
        if editable_setter:
            def on_editing(label, _pspec):
                if not label.get_editing():
                    try:
                        editable_setter(item, label.get_text())
                    except ValueError:
                        label.set_text(getter(item))
            list_item._hid = w.connect("notify::editing", on_editing)
        else:
            # Keep read-only cells (totals) in sync when qty changes
            def refresh(*_):
                w.set_text(getter(item))
            list_item._hid = item.connect("notify::qty", refresh)

    def unbind(_f, list_item):
        target = list_item.get_child() if editable_setter else list_item.get_item()
        target.disconnect(list_item._hid)

    factory.connect("setup", setup)
    factory.connect("bind", bind)
    factory.connect("unbind", unbind)
    col = Gtk.ColumnViewColumn(title=title, factory=factory)
    col.set_resizable(True)
    return col


def set_qty(item, text):
    item.qty = int(text)


def on_activate(app):
    store = Gio.ListStore(item_type=Item)
    for args in [("Pump", "Centrifugal pump", 2, 45.5),
                 ("Valve", "Gate valve DN50", 10, 6.2),
                 ("Cable", "Cable drum", 3, 120.0)]:
        store.append(Item(*args))

    view = Gtk.ColumnView(model=Gtk.SingleSelection(model=store))
    view.set_show_column_separators(True)
    view.set_show_row_separators(True)
    view.append_column(text_column("Item", lambda i: i.name))
    view.append_column(text_column("Description", lambda i: i.desc))
    view.append_column(text_column("Qty", lambda i: str(i.qty), set_qty))
    view.append_column(text_column("Unit wt (kg)", lambda i: f"{i.weight:.1f}"))
    view.append_column(text_column("Total wt (kg)", lambda i: f"{i.qty * i.weight:.1f}"))

    scroll = Gtk.ScrolledWindow(vexpand=True)
    scroll.set_child(view)

    win = Gtk.ApplicationWindow(application=app, title="Load List (GTK4)",
                                default_width=700, default_height=400)
    win.set_child(scroll)
    win.present()


app = Gtk.Application(application_id="org.example.LoadList")
app.connect("activate", on_activate)
app.run()
# Mako colour themes

Mako comes with 14 colour themes (Preferences → Appearance → Color Theme).
You can make your own too: a theme is one small CSS file.

## Make a theme

1. Open **Preferences → Appearance** and click **New Theme**. Mako copies
   [`template.css`](template.css) to `~/.config/mako/themes/my-theme.css`
   and opens it in your text editor.
2. Change the colours and save. The window updates on every save, so keep
   it open next to the editor.
3. Give it a name on the `Name:` line at the top. It shows under that name
   in the picker, with a small preview in its colours.

To use someone else's theme, drop the `.css` file into
`~/.config/mako/themes/`. **Open Themes Folder** in Preferences takes you
there.

## What goes in the file

Plain GTK CSS, written against the class `.mako-theme`. Mako puts that
class on the window while the theme is chosen. Most themes only set colour
variables:

```css
.mako-theme {
  --view-bg-color: #1e2030;     /* behind the files */
  --mako-sidebar: #222436;      /* the sidebar */
  --accent-bg-color: #82aaff;   /* selected tab, selection, switches */
}

@media (prefers-color-scheme: light) {
  .mako-theme {
    --view-bg-color: #e1e2e7;
    --mako-sidebar: #d5d6db;
    --accent-bg-color: #2e7de9;
  }
}
```

Anything you leave out stays as GNOME has it. [`template.css`](template.css)
lists every variable worth setting, each with a note on what it colours.
It also has a few switched-off rules that go beyond colours, such as a
painted header bar, rounder tabs and your own zebra stripes.

| Variable | Colours |
| --- | --- |
| `--view-bg-color`, `--view-fg-color` | The file views: list, grid, columns |
| `--window-bg-color`, `--window-fg-color` | The window around them, the gaps between framed panels |
| `--mako-sidebar`, `--sidebar-bg-color`, `--sidebar-fg-color` | The sidebar |
| `--headerbar-bg-color`, `--headerbar-fg-color` | Header bars |
| `--popover-bg-color`, `--dialog-bg-color`, `--card-bg-color` (and `-fg-`) | Menus, dialogs, cards |
| `--accent-bg-color`, `--accent-fg-color`, `--accent-color` | The accent |
| `--mako-tile-bg` (on `.mako-theme.framed-panels`) | Each panel's card with Framed Panels on |

The full list of GNOME's variables is in the
[libadwaita docs](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/css-variables.html).

## Examples

- [`examples/paper.css`](examples/paper.css): warm paper in light mode,
  ink at night, plus a painted header bar and rounder tabs.

## When something doesn't work

Start Mako from a terminal with `mako`. A mistake in a theme is printed
there with the file and line. To find the name of a part you want to
style, run `GTK_DEBUG=interactive mako` and use the GTK Inspector's
picker.

Made a theme you like? Share it in
[Discussions](https://github.com/tommkotzu/mako/discussions).

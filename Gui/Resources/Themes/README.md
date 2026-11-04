# Natron themes

Each sub-folder is a theme: a Qt stylesheet (`.qss`) that Natron loads from
**Preferences > Appearance > Stylesheet file**. Natron fills the `%1`..`%11`
placeholders with the colours set in the same Preferences page, so a theme is
the stylesheet plus a set of colours.

The fork's install puts this folder at the root of the install
(`<Natron>/Themes/`), next to `bin/`, `Plugins/` and `Resources/`.

| Theme | Look |
|---|---|
| `nuketron` | Nuke-like, based on a user-made `mainstyle_NUKE.qss` (flat square tabs and tooltips, gradient buttons, thin splitters, light frame around the selected properties panel): Nuke greys, orange selection / active tabs / menus, yellow section titles, pale tooltips, flat grey timeline with an orange playhead |

## Applying a theme

Preferences > Appearance > **Stylesheet file** = `<Natron>/Themes/nuketron/nuketron.qss`,
then Save. Clear the field to go back to Natron's look. The theme hard-codes its
greys; the few colours it takes from Preferences (selection, keyframes...) keep
their Natron defaults, which already use Nuke's orange.

## Writing a theme

Start from `nuketron/nuketron.qss` (Natron's `Gui/Resources/Stylesheets/mainstyle.qss`
plus an overrides block at the end). Never write a bare percent sign in a `.qss`:
Natron fills the placeholders with `QString::arg`, which would eat it.
The timeline, curve editor, dope sheet and node graph are drawn with OpenGL /
QPainter from the Preferences colours, not from the stylesheet.

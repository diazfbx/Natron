/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_GUI_ICONTINT_H
#define NATRON_GUI_ICONTINT_H

// Light icons for dark toolbars (viewer playback bar): the stock player
// icons are black glyphs, nearly invisible on a #323232 panel. Grey pixels
// get their brightness inverted into a light-grey range; coloured pixels
// (red stop, orange accents...) are kept as they are.

#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QAbstractButton>
#include <QWidget>
#include <QList>
#include <algorithm>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

inline QPixmap
natronLightPixmap(const QPixmap& src)
{
    if ( src.isNull() ) {
        return src;
    }
    QImage img = src.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>( img.scanLine(y) );
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = line[x];
            const int a = qAlpha(p);
            if (a == 0) {
                continue;
            }
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            const int mx = std::max(r, std::max(g, b));
            const int mn = std::min(r, std::min(g, b));
            if (mx - mn > 48) {
                continue;   // coloured detail: keep
            }
            const int lum = (r + g + b) / 3;
            const int v = 230 - (lum * 110) / 255;   // black -> 230, white -> 120
            line[x] = qRgba(v, v, v, a);
        }
    }
    return QPixmap::fromImage(img);
}

inline QIcon
natronLightIcon(const QIcon& icon, const QSize& size)
{
    if ( icon.isNull() ) {
        return icon;
    }
    QIcon out;
    const QIcon::State states[2] = { QIcon::Off, QIcon::On };
    const QIcon::Mode modes[2] = { QIcon::Normal, QIcon::Disabled };
    for (int s = 0; s < 2; ++s) {
        for (int m = 0; m < 2; ++m) {
            QPixmap pix = icon.pixmap(size, modes[m], states[s]);
            if ( !pix.isNull() ) {
                out.addPixmap(natronLightPixmap(pix), modes[m], states[s]);
            }
        }
    }
    return out;
}

// Lighten the icon of every button inside container (once: buttons are tagged).
inline void
natronLightenButtonIcons(QWidget* container)
{
    if (!container) {
        return;
    }
    QList<QAbstractButton*> buttons = container->findChildren<QAbstractButton*>();
    for (int i = 0; i < buttons.size(); ++i) {
        QAbstractButton* b = buttons[i];
        if ( b->icon().isNull() ) {
            continue;
        }
        QSize sz = b->iconSize();
        if ( !sz.isValid() || sz.isEmpty() ) {
            sz = QSize(32, 32);
        }
        b->setIcon( natronLightIcon(b->icon(), sz * 2) );
    }
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_ICONTINT_H

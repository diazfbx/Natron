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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "PyGlobalGui.h"

#include <algorithm>
#include <string>

#include <QTimer>
#include <QApplication>
#include <QDebug>

#include "Engine/AppManager.h"   // interpretPythonScript

NATRON_NAMESPACE_ENTER;
NATRON_PYTHON_NAMESPACE_ENTER;

void
PyGuiApplication::callLater(int msec,
                            const QString& pythonFunctionName)
{
    const std::string fn = pythonFunctionName.trimmed().toStdString();
    if ( fn.empty() ) {
        return;
    }
    QTimer::singleShot( std::max(0, msec), qApp, [fn]() {
        std::string err;
        // interpretPythonScript takes the GIL; runs in __main__ like menu commands.
        NATRON_PYTHON_NAMESPACE::interpretPythonScript(fn + "()", &err, NULL);
        if ( !err.empty() ) {
            qDebug() << "callLater(" << fn.c_str() << "):" << err.c_str();
        }
    } );
}

NATRON_PYTHON_NAMESPACE_EXIT;
NATRON_NAMESPACE_EXIT;

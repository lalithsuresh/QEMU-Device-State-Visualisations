/*
 * QBuffer Module
 *
 * Copyright (C) 2010 Siemens AG
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef QBUFFER_H
#define QBUFFER_H

#include <stdint.h>
#include "qobject.h"
#include "qstring.h"

typedef struct QBuffer {
    QObject_HEAD;
    void *data;
    size_t size;
} QBuffer;

QBuffer *qbuffer_from_data(const void *data, size_t size);
QBuffer *qbuffer_from_qstring(const QString *string);
const void *qbuffer_get_data(const QBuffer *qb);
size_t qbuffer_get_size(const QBuffer *qb);
QBuffer *qobject_to_qbuffer(const QObject *obj);

#endif /* QBUFFER_H */

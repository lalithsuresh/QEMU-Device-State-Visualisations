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

#include "qbuffer.h"
#include "qobject.h"
#include "qemu-common.h"
#include "base64.h"

static void qbuffer_destroy_obj(QObject *obj);

static const QType qbuffer_type = {
    .code = QTYPE_QBUFFER,
    .destroy = qbuffer_destroy_obj,
};

/**
 * qbuffer_from_data(): Create a new QBuffer from an existing data blob
 *
 * Returns strong reference.
 */
QBuffer *qbuffer_from_data(const void *data, size_t size)
{
    QBuffer *qb;

    qb = qemu_malloc(sizeof(*qb));
    qb->data = qemu_malloc(size);
    memcpy(qb->data, data, size);
    qb->size = size;
    QOBJECT_INIT(qb, &qbuffer_type);

    return qb;
}

/**
 * qbuffer_from_qstring(): Create a new QBuffer from a QString object that
 * contains the data as a stream of hex-encoded bytes
 *
 * Returns strong reference.
 */
QBuffer *qbuffer_from_qstring(const QString *string)
{
    const char *str = qstring_get_str(string);
    size_t str_len;
    QBuffer *qb;

    qb = qemu_malloc(sizeof(*qb));

    str_len = strlen(str);
    while (str_len > 0 && str[str_len - 1] == '=') {
        str_len--;
    }
    qb->size = (str_len / 4) * 3 + ((str_len % 4) * 3) / 4;
    qb->data = qemu_malloc(qb->size);

    QOBJECT_INIT(qb, &qbuffer_type);

    if (base64_decode(str, str_len, qb->data) < 0) {
        qbuffer_destroy_obj(QOBJECT(qb));
        return NULL;
    }

    return qb;
}

/**
 * qbuffer_get_data(): Return pointer to stored data
 *
 * NOTE: Should be used with caution, if the object is deallocated
 * this pointer becomes invalid.
 */
const void *qbuffer_get_data(const QBuffer *qb)
{
    return qb->data;
}

/**
 * qbuffer_get_size(): Return size of stored data
 */
size_t qbuffer_get_size(const QBuffer *qb)
{
    return qb->size;
}

/**
 * qobject_to_qbool(): Convert a QObject into a QBuffer
 */
QBuffer *qobject_to_qbuffer(const QObject *obj)
{
    if (qobject_type(obj) != QTYPE_QBUFFER)
        return NULL;

    return container_of(obj, QBuffer, base);
}

/**
 * qbuffer_destroy_obj(): Free all memory allocated by a QBuffer object
 */
static void qbuffer_destroy_obj(QObject *obj)
{
    QBuffer *qb;

    assert(obj != NULL);
    qb = qobject_to_qbuffer(obj);
    qemu_free(qb->data);
    qemu_free(qb);
}

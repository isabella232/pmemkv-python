/*
 * Copyright 2019-2020, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#define PY_SSIZE_T_CLEAN

#include <Python.h>
#include "structmember.h"
#include <string>
#include <libpmemkv.h>
#include <libpmemkv_json_config.h>
#include <iostream>
#include <unordered_map>

#ifdef __cplusplus
extern "C" {
#endif

static PyObject *PmemkvException;

typedef struct {
	PyObject *exception;
	const char *object_name;
	const char *exception_name;
	const char *docstring;
} Exception;

static std::unordered_map<int, Exception> ExceptionDispatcher = {
	{PMEMKV_STATUS_UNKNOWN_ERROR,
	 Exception{NULL, "UnknownError", "pmemkv_NI.UnknownError",
		   "Something unexpected happened"}},
	{PMEMKV_STATUS_NOT_FOUND,
	 Exception{PyExc_KeyError, NULL, NULL,
		   "Database entry or config item not found"}},
	{PMEMKV_STATUS_NOT_SUPPORTED,
	 Exception{NULL, "NotSupported", "pmemkv_NI.NotSupported",
		   "Function is not implemented by current engine"}},
	{PMEMKV_STATUS_INVALID_ARGUMENT,
	 Exception{NULL, "InvalidArgument", "pmemkv_NI.InvalidArgument",
		   "Argument to function has wrong value"}},
	{PMEMKV_STATUS_CONFIG_PARSING_ERROR,
	 Exception{NULL, "ConfigParsingError", "pmemkv_NI.ConfigParsingError",
		   "Processing config failed"}},
	{PMEMKV_STATUS_CONFIG_TYPE_ERROR,
	 Exception{NULL, "ConfigTypeError", "pmemkv_NI.ConfigTypeError",
		   "Config item has different type than expected"}},
	{PMEMKV_STATUS_STOPPED_BY_CB,
	 Exception{NULL, "StoppedByCallback", "pmemkv_NI.StoppedByCallback",
		   "Callback function aborted in an unexpected way"}},
	{PMEMKV_STATUS_OUT_OF_MEMORY,
	 Exception{
		 PyExc_MemoryError, NULL, NULL,
		 "Operation failed because there is not enough memory (or space on the device)"}},
	{PMEMKV_STATUS_WRONG_ENGINE_NAME,
	 Exception{NULL, "WrongEngineName", "pmemkv_NI.WrongEngineName",
		   "Engine name does not match any available engine"}},
	{PMEMKV_STATUS_TRANSACTION_SCOPE_ERROR,
	 Exception{
		 NULL, "TransactionScopeError", "pmemkv_NI.TransactionScopeError",
		 "An error with the scope of the libpmemobj transaction. This exception is defined for compatibility with pmemkv API and probably will never occur"}},
};

static const char *memory_exception_msg = "Cannot allocate memory for internal objects";

typedef struct {
	PyObject_HEAD
	const char *value;
	Py_ssize_t length;
} PmemkvValueBufferObject;

static PyMemberDef PmemvValueBuffer_members[] = {
	{"value", T_INT, offsetof(PmemkvValueBufferObject, value), 0, "Pointer to data"},
	{"length", T_INT, offsetof(PmemkvValueBufferObject, length), 0,
	 "Length of underlying data"},
	{NULL}};

static int PmemkvValueBufferObject_getbuffer(PyObject *obj, Py_buffer *view, int flags)
{
	if (view == NULL) {
		PyErr_SetString(PyExc_ValueError, "NULL view in getbuffer");
		return -1;
	}
	PmemkvValueBufferObject *self = (PmemkvValueBufferObject *)obj;
	view->obj = obj;
	view->buf = (void *)self->value;
	view->len = self->length;
	view->readonly = 1;
	view->itemsize = 1;
	view->format = 0; // unsigned bytes
	view->ndim = 1;
	view->shape = NULL;
	view->strides = NULL;
	view->suboffsets = NULL;
	view->internal = NULL;
	Py_INCREF(self);
	return 0;
}

static PyBufferProcs PmemkvValueBuffer_as_buffer = {
	// this definition is only compatible with Python 3.3 and above
	(getbufferproc)PmemkvValueBufferObject_getbuffer,
	(releasebufferproc)0, // we do not require any special release function
};

static PyObject *PmemkvValueBuffer_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PmemkvValueBufferObject *self =
		(PmemkvValueBufferObject *)type->tp_alloc(type, 0);
	return (PyObject *)self;
}

static int PmemkvValueBuffer_init(PmemkvValueBufferObject *self)
{
	if (self != NULL) {
		self->value = NULL;
		self->length = 0;
	} else {
		return -1;
	}
	return 0;
}

static void PmemkvValueBuffer_dealloc(PmemkvValueBufferObject *self)
{
	PyObject_Del(self);
}

/*
 * Configuration of PmemkvValueBuffer object.
 */
static PyTypeObject PmemkvValueBufferType = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
	.tp_name = "pmemkv.pmemkv_NI",
	.tp_basicsize = sizeof(PmemkvValueBufferObject),
	.tp_dealloc = (destructor)PmemkvValueBuffer_dealloc,
	.tp_as_buffer = &PmemkvValueBuffer_as_buffer,
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc = "Pmemkv value type",
	.tp_members = PmemvValueBuffer_members,
	.tp_init = (initproc)PmemkvValueBuffer_init,
	.tp_new = PmemkvValueBuffer_new,
};

typedef struct {
	PyObject_HEAD
	pmemkv_db *db;
} PmemkvObject;

static PyMemberDef
pmemkv_NI_members[] = {
	{"db", T_INT, offsetof(PmemkvObject, db), 0, "Engine instance"},
	{NULL}
};

static PyObject *
Pmemkv_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
	PmemkvObject *self = (PmemkvObject *) type->tp_alloc(type, 0);
	return (PyObject *) self;
}

// Turn on/off operations.
static PyObject *
pmemkv_NI_Start(PmemkvObject *self, PyObject* args) {
	Py_buffer engine, json_config;
	if (!PyArg_ParseTuple(args, "s*s*", &engine, &json_config)) {
		return NULL;
	}

	pmemkv_config *config = pmemkv_config_new();
	if (config == nullptr) {
		// "Allocating a new pmemkv config failed"
		PyErr_SetString(PmemkvException, pmemkv_errormsg());
		return NULL;
	}

	int rv = pmemkv_config_from_json(config, (const char*) json_config.buf);
	if (rv != PMEMKV_STATUS_OK) {
		pmemkv_config_delete(config);
		// "Creating a pmemkv config from JSON string failed"
		PyErr_SetString(ExceptionDispatcher[rv].exception,
				pmemkv_config_from_json_errormsg());
		return NULL;
	}

	rv = pmemkv_open((const char*) engine.buf, config, &self->db);
	if (rv != PMEMKV_STATUS_OK) {
		// "pmemkv_open failed"
		PyErr_SetString(ExceptionDispatcher[rv].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *
pmemkv_NI_Stop(PmemkvObject *self) {
	if( self->db != NULL)
		pmemkv_close(self->db);
	self->db = NULL;
	Py_RETURN_NONE;
}

static void
Pmemkv_dealloc(PmemkvObject *self) {
    pmemkv_NI_Stop(self);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

void value_callback(const char *value, size_t valuebyte, void *context)
{
	PmemkvValueBufferObject *entry =
		PyObject_New(PmemkvValueBufferObject, &PmemkvValueBufferType);
	if (entry == NULL) {

		PyErr_SetString(PyExc_MemoryError, memory_exception_msg);
		return;
	}
	entry->value = value;
	entry->length = valuebyte;
	PyObject *args = PyTuple_New(1);
	if (args == NULL) {
		Py_DECREF(entry);
		PyErr_SetString(PyExc_MemoryError, memory_exception_msg);
		return;
	}
	// PyTuple_SetItem sets en exception on failure on its own
	if (PyTuple_SetItem(args, 0, (PyObject *)entry) == 0) {
		PyObject *res = PyObject_CallObject((PyObject *)context, args);
		Py_XDECREF(res);
	}
	Py_XDECREF(args); // args is the owner of the entry reference counter
}

int key_callback(const char *key, size_t keybytes, const char *value, size_t valuebyte,
		 void *context)
{
	value_callback(key, keybytes, context);
	if (PyErr_Occurred() != NULL)
		return -1;
	return 0;
}

int key_value_callback(const char *key, size_t keybytes, const char *value,
		       size_t valuebyte, void *context)
{
	PmemkvValueBufferObject *value_buffer =
		PyObject_New(PmemkvValueBufferObject, &PmemkvValueBufferType);
	PmemkvValueBufferObject *key_buffer =
		PyObject_New(PmemkvValueBufferObject, &PmemkvValueBufferType);
	if ((value_buffer == NULL) || (key_buffer == NULL)) {
		Py_XDECREF(value_buffer);
		Py_XDECREF(key_buffer);
		PyErr_SetString(PyExc_MemoryError, memory_exception_msg);
		return -1;
	}
	value_buffer->value = value;
	value_buffer->length = valuebyte;
	key_buffer->value = key;
	key_buffer->length = keybytes;

	PyObject *args = PyTuple_New(2);
	if (args == NULL) {
		Py_DECREF(value_buffer);
		Py_DECREF(key_buffer);
		return -1;
	}
	// PyTuple_SetItem sets an exception on failure on its own
	if ((PyTuple_SetItem(args, 0, (PyObject *)key_buffer) == 0) &&
	    (PyTuple_SetItem(args, 1, (PyObject *)value_buffer) == 0)) {
		PyObject *res = PyObject_CallObject((PyObject *)context, args);
		Py_XDECREF(res);
	}
	key_buffer->value = NULL;
	key_buffer->length = 0;
	value_buffer->value = NULL;
	value_buffer->length = 0;

	Py_DECREF(args); // args is the owner of the contained objects reference counters

	if (PyErr_Occurred() != NULL)
		return -1;
	return 0;
}

// "All" Methods.
static PyObject *
pmemkv_NI_GetKeys(PmemkvObject *self, PyObject* args) {
	PyObject* python_callback;
	if (!PyArg_ParseTuple(args, "O:set_callback", &python_callback)) {
		return NULL;
	}
	int result = pmemkv_get_all(self->db, key_callback, python_callback);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *
pmemkv_NI_GetKeysAbove(PmemkvObject *self, PyObject* args) {
	Py_buffer key;
	PyObject* python_callback;
	if (!PyArg_ParseTuple(args, "s*O:set_callback", &key, &python_callback)) {
		return NULL;
	}
	int result = pmemkv_get_above(self->db, (const char *)key.buf, key.len,
				      key_callback, python_callback);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *
pmemkv_NI_GetKeysBelow(PmemkvObject *self, PyObject* args) {
	Py_buffer key;
	PyObject* python_callback;
	if (!PyArg_ParseTuple(args, "s*O:set_callback", &key, &python_callback)) {
		return NULL;
	}
	int result = pmemkv_get_below(self->db, (const char *)key.buf, key.len,
				      key_callback, python_callback);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *
pmemkv_NI_GetKeysBetween(PmemkvObject *self, PyObject* args) {
	Py_buffer key1, key2;
	PyObject* python_callback;
	if (!PyArg_ParseTuple(args, "s*s*O:set_callback", &key1, &key2, &python_callback)) {
		return NULL;
	}
	int result = pmemkv_get_between(self->db, (const char *)key1.buf, key1.len,
					(const char *)key2.buf, key2.len, key_callback,
					python_callback);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

// "Count" Methods.
static PyObject *
pmemkv_NI_CountAll(PmemkvObject *self) {
	size_t cnt;
	int result = pmemkv_count_all(self->db, &cnt);
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	return Py_BuildValue("i", cnt);
}

static PyObject *
pmemkv_NI_CountAbove(PmemkvObject *self, PyObject* args) {
	Py_buffer key;
	if (!PyArg_ParseTuple(args, "s*", &key)) {
		return NULL;
	}
	size_t cnt;
	int result = pmemkv_count_above(self->db, (const char*) key.buf, key.len, &cnt);
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	return Py_BuildValue("i", cnt);
}

static PyObject *
pmemkv_NI_CountBelow(PmemkvObject *self, PyObject* args) {
	Py_buffer key;
	if (!PyArg_ParseTuple(args, "s*", &key)) {
		return NULL;
	}
	size_t cnt;
	int result = pmemkv_count_below(self->db, (const char*) key.buf, key.len, &cnt);
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	return Py_BuildValue("i", cnt);
}

static PyObject *
pmemkv_NI_CountBetween(PmemkvObject *self, PyObject* args) {
	Py_buffer key1, key2;
	if (!PyArg_ParseTuple(args, "s*s*", &key1, &key2)) {
		return NULL;
	}
	size_t cnt;
	int result = pmemkv_count_between(self->db, (const char*) key1.buf, key1.len, (const char*) key2.buf, key2.len, &cnt);
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	return Py_BuildValue("i", cnt);
}

// "Each" Methods.
static PyObject *
pmemkv_NI_GetAll(PmemkvObject *self, PyObject* args) {
	PyObject* python_callback;
	if (!PyArg_ParseTuple(args, "O:set_callback", &python_callback)) {
		return NULL;
	}
	int result = pmemkv_get_all(self->db, key_value_callback, python_callback);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *
pmemkv_NI_GetAbove(PmemkvObject *self, PyObject* args) {
	Py_buffer key;
	PyObject* python_callback;
	if (!PyArg_ParseTuple(args, "s*O:set_callback", &key, &python_callback)) {
		return NULL;
	}
	int result = pmemkv_get_above(self->db, (const char *)key.buf, key.len,
				      key_value_callback, python_callback);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *
pmemkv_NI_GetBelow(PmemkvObject *self, PyObject* args) {
	Py_buffer key;
	PyObject* python_callback;
	if (!PyArg_ParseTuple(args, "s*O:set_callback", &key, &python_callback)) {
		return NULL;
	}
	int result = pmemkv_get_below(self->db, (const char *)key.buf, key.len,
				      key_value_callback, python_callback);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *
pmemkv_NI_GetBetween(PmemkvObject *self, PyObject* args) {
	Py_buffer key1, key2;
	PyObject* python_callback;
	if (!PyArg_ParseTuple(args, "s*s*O:set_callback", &key1, &key2, &python_callback)) {
		return NULL;
	}
	int result = pmemkv_get_between(self->db, (const char *)key1.buf, key1.len,
					(const char *)key2.buf, key2.len,
					key_value_callback, python_callback);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

// "Exists" Method.
static PyObject *
pmemkv_NI_Exists(PmemkvObject *self, PyObject* args) {
	Py_buffer key;
	if (!PyArg_ParseTuple(args, "s*", &key)) {
		return NULL;
	}
	int result = pmemkv_exists(self->db, (const char*) key.buf, key.len);
	if (result != PMEMKV_STATUS_OK && result != PMEMKV_STATUS_NOT_FOUND) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	return PyBool_FromLong(result == PMEMKV_STATUS_OK);
}

// "CRUD" Operations.
static PyObject *
pmemkv_NI_Put(PmemkvObject *self, PyObject* args) {
	Py_buffer key, value;
	if (!PyArg_ParseTuple(args, "s*s*", &key, &value)) {
		return NULL;
	}
	int result = pmemkv_put(self->db, (const char*) key.buf, key.len, (const char*) value.buf, value.len);
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *pmemkv_NI_GetString(PmemkvObject *self, PyObject *args)
{
	Py_buffer key;
	if (!PyArg_ParseTuple(args, "s*", &key)) {
		return NULL;
	}
	struct GetCallbackContext {
		int status;
		std::string value;
	};
	GetCallbackContext cxt = {PMEMKV_STATUS_NOT_FOUND, ""};

	auto callback = [](const char* v, size_t vb, void* context) {
		const auto c = ((GetCallbackContext*) context);
		c->status = PMEMKV_STATUS_OK;
		c->value.append(v, vb);
	};
	int result = pmemkv_get(self->db, (const char*) key.buf, key.len, callback, &cxt);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	} else if (cxt.status == PMEMKV_STATUS_OK) {
		return Py_BuildValue("s#", cxt.value.data(), cxt.value.size());
	}
	Py_RETURN_NONE;
}

static PyObject *pmemkv_NI_Get(PmemkvObject *self, PyObject *args)
{
	Py_buffer key;
	PyObject *python_callback;
	if (!PyArg_ParseTuple(args, "s*O:set_callback", &key, &python_callback)) {
		return NULL;
	}
	int result = pmemkv_get(self->db, (const char *)key.buf, key.len, value_callback,
				python_callback);
	if (PyErr_Occurred() != NULL)
		return NULL;
	if (result != PMEMKV_STATUS_OK) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *
pmemkv_NI_Remove(PmemkvObject *self, PyObject* args) {
	Py_buffer key;
	if (!PyArg_ParseTuple(args, "s*", &key)) {
		return NULL;
	}
	int result = pmemkv_remove(self->db, (const char*) key.buf, key.len);
	if (result != PMEMKV_STATUS_OK && result != PMEMKV_STATUS_NOT_FOUND) {
		PyErr_SetString(ExceptionDispatcher[result].exception, pmemkv_errormsg());
		return NULL;
	}
	return PyBool_FromLong(result == PMEMKV_STATUS_OK);
}

// Functions declarations.
static PyMethodDef pmemkv_NI_methods[] = {
	{"start", (PyCFunction)pmemkv_NI_Start, METH_VARARGS, NULL},
	{"stop", (PyCFunction)pmemkv_NI_Stop, METH_NOARGS, NULL},
	{"put", (PyCFunction)pmemkv_NI_Put, METH_VARARGS, NULL},
	{"get_string", (PyCFunction)pmemkv_NI_GetString, METH_VARARGS, NULL},
	{"get", (PyCFunction)pmemkv_NI_Get, METH_VARARGS, NULL},
	{"get_keys", (PyCFunction)pmemkv_NI_GetKeys, METH_VARARGS, NULL},
	{"get_keys_above", (PyCFunction)pmemkv_NI_GetKeysAbove, METH_VARARGS, NULL},
	{"get_keys_below", (PyCFunction)pmemkv_NI_GetKeysBelow, METH_VARARGS, NULL},
	{"get_keys_between", (PyCFunction)pmemkv_NI_GetKeysBetween, METH_VARARGS, NULL},
	{"count_all", (PyCFunction)pmemkv_NI_CountAll, METH_NOARGS, NULL},
	{"count_above", (PyCFunction)pmemkv_NI_CountAbove, METH_VARARGS, NULL},
	{"count_below", (PyCFunction)pmemkv_NI_CountBelow, METH_VARARGS, NULL},
	{"count_between", (PyCFunction)pmemkv_NI_CountBetween, METH_VARARGS, NULL},
	{"get_all", (PyCFunction)pmemkv_NI_GetAll, METH_VARARGS, NULL},
	{"get_above", (PyCFunction)pmemkv_NI_GetAbove, METH_VARARGS, NULL},
	{"get_below", (PyCFunction)pmemkv_NI_GetBelow, METH_VARARGS, NULL},
	{"get_between", (PyCFunction)pmemkv_NI_GetBetween, METH_VARARGS, NULL},
	{"exists", (PyCFunction)pmemkv_NI_Exists, METH_VARARGS, NULL},
	{"remove", (PyCFunction)pmemkv_NI_Remove, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}};

/*
 * Configuration of pmemkv_NI object.
 */
static PyTypeObject PmemkvType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "pmemkv.pmemkv_NI",
	.tp_basicsize = sizeof(PmemkvObject),
	.tp_dealloc = (destructor)Pmemkv_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc = "Pmemkv binding",
	.tp_methods = pmemkv_NI_methods,
	.tp_members = pmemkv_NI_members,
	.tp_new = Pmemkv_new,
};

// Module definition.
static struct PyModuleDef pmemkv_NI_module = {
	PyModuleDef_HEAD_INIT,
	"_pmemkv", /* name of the module */
	NULL, /* module documentation, may be NULL */
	-1, /* size of per-interpreter state of the module, or -1 if the module keeps state in global variables. */
};

// Creating dynamic module.
PyMODINIT_FUNC
PyInit__pmemkv(void) {
	PyObject *m;
	if (PyType_Ready(&PmemkvType) < 0)
		return NULL;

	m = PyModule_Create(&pmemkv_NI_module);
	if (m == NULL)
		return NULL;

	try {
		Py_INCREF(&PmemkvType);
		if (PyModule_AddObject(m, "pmemkv_NI", (PyObject *)&PmemkvType) < 0) {
			throw;
		}
		PmemkvException =
			PyErr_NewException("pmemkv_NI.PmemkvException", NULL, NULL);
		if (PyModule_AddObject(m, "Error", PmemkvException) < 0) {
			throw;
		}
		for (auto &e : ExceptionDispatcher) {
			if (e.second.exception == NULL) {
				e.second.exception = PyErr_NewExceptionWithDoc(
					e.second.exception_name, e.second.docstring,
					PmemkvException, NULL);
				if (PyModule_AddObject(m, e.second.object_name,
						       e.second.exception) < 0) {
					throw;
				}
			}
		}
	} catch (...) {
		Py_XDECREF(&PmemkvType);
		Py_XDECREF(m);
		Py_XDECREF(PmemkvException);
		Py_XDECREF(m);
		for (auto &e : ExceptionDispatcher) {
			Py_XDECREF(e.second.exception);
			Py_XDECREF(m);
		}
		return NULL;
	}
	return m;
}

#ifdef __cplusplus
}
#endif

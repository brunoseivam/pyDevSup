
/* python has its own ideas about which version to support */
#undef _POSIX_C_SOURCE
#undef _XOPEN_SOURCE

#include <Python.h>
#ifdef HAVE_NUMPY
#include <numpy/ndarrayobject.h>
#endif

#include <epicsVersion.h>
#include <dbCommon.h>
#include <dbAccess.h>
#include <dbStaticLib.h>
#include <recSup.h>
#include <special.h>

#include "pydevsup.h"

#ifdef HAVE_NUMPY
static int dbf2np_map[DBF_MENU+1] = {
    NPY_BYTE,
    NPY_BYTE,
    NPY_UBYTE,
    NPY_INT16,
    NPY_UINT16,
    NPY_INT32,
    NPY_UINT32,
    NPY_FLOAT32,
    NPY_FLOAT64,
    NPY_INT16,
    NPY_INT16,
};
static PyArray_Descr* dbf2np[DBF_MENU+1];
#endif

typedef struct {
    PyObject_HEAD

    DBADDR addr;
} pyField;

static int pyField_Init(pyField *self, PyObject *args, PyObject *kws)
{
    const char *pvname;

    if(!PyArg_ParseTuple(args, "s", &pvname))
        return -1;

    if(dbNameToAddr(pvname, &self->addr)) {
        PyErr_Format(PyExc_ValueError, "%s: Record field not found", pvname);
        return -1;
    }

    return 0;
}

static void exc_wrong_ftype(pyField *self)
{
    PyErr_Format(PyExc_TypeError, "Operation on field type %d is not supported",
                 self->addr.field_type);
}

static PyObject* pyField_name(pyField *self)
{
    return Py_BuildValue("ss",
                         self->addr.precord->name,
                         self->addr.pfldDes->name);
}

static PyObject* pyField_fldinfo(pyField *self)
{
    short dbf=self->addr.field_type;
    short fsize=self->addr.field_size;
    unsigned long nelm=self->addr.no_elements;
    return Py_BuildValue("hhk", dbf, fsize, nelm);
}

static PyObject* pyField_getval(pyField *self)
{
    switch(self->addr.field_type)
    {
#define OP(FTYPE, CTYPE, FN) case DBF_##FTYPE: return FN(*(CTYPE*)self->addr.pfield)
    OP(CHAR,  epicsInt8,   PyInt_FromLong);
    OP(UCHAR, epicsUInt8,  PyInt_FromLong);
    OP(ENUM,  epicsEnum16, PyInt_FromLong);
    OP(MENU,  epicsEnum16, PyInt_FromLong);
    OP(SHORT, epicsInt16,  PyInt_FromLong);
    OP(USHORT,epicsUInt16, PyInt_FromLong);
    OP(LONG,  epicsInt32,  PyInt_FromLong);
    OP(ULONG, epicsUInt32, PyInt_FromLong);
    OP(FLOAT, epicsFloat32,PyFloat_FromDouble);
    OP(DOUBLE,epicsFloat64,PyFloat_FromDouble);
#undef OP
    case DBF_STRING:
        return PyString_FromString((char*)self->addr.pfield);
    default:
        exc_wrong_ftype(self);
        return NULL;
    }
}

static PyObject* pyField_putval(pyField *self, PyObject* args)
{
    PyObject *val;

    if(!PyArg_ParseTuple(args, "O", &val))
        return NULL;

    if(val==Py_None) {
        PyErr_Format(PyExc_ValueError, "Can't assign None to %s.%s",
                     self->addr.precord->name,
                     self->addr.pfldDes->name);
        return NULL;
    }

    switch(self->addr.field_type)
    {
#define OP(FTYPE, CTYPE, FN) case DBF_##FTYPE: *(CTYPE*)self->addr.pfield = FN(val); break
    OP(CHAR,  epicsInt8,   PyInt_AsLong);
    OP(UCHAR, epicsUInt8,  PyInt_AsLong);
    OP(ENUM,  epicsEnum16, PyInt_AsLong);
    OP(MENU,  epicsEnum16, PyInt_AsLong);
    OP(SHORT, epicsInt16,  PyInt_AsLong);
    OP(USHORT,epicsUInt16, PyInt_AsLong);
    OP(LONG,  epicsInt32,  PyInt_AsLong);
    OP(ULONG, epicsUInt32, PyInt_AsLong);
    OP(FLOAT, epicsFloat32,PyFloat_AsDouble);
    OP(DOUBLE,epicsFloat64,PyFloat_AsDouble);
#undef OP
    case DBF_STRING: {
        const char *fld;
        char *dest=self->addr.pfield;
#if PY_MAJOR_VERSION >= 3
        PyObject *data = PyUnicode_AsEncodedString(val, "ascii", "Encoding error:");
        if(!data)
            return NULL;
        fld = PyUnicode_AS_DATA(data);
#else
        fld = PyString_AsString(val);
#endif
        if(fld) {
          strncpy(dest, fld, MAX_STRING_SIZE);
          dest[MAX_STRING_SIZE-1]='\0';
        } else {
          dest[0] = '\0';
        }
#if PY_MAJOR_VERSION >= 3
        Py_DECREF(data);
#endif
        break;
    }
    default:
        exc_wrong_ftype(self);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *pyField_getarray(pyField *self)
{
#ifdef HAVE_NUMPY
    rset *prset;
    int flags = NPY_CARRAY;
    char *data;
    npy_intp dims[1] = {self->addr.no_elements};
    PyArray_Descr *desc;

    if(self->addr.special==SPC_DBADDR &&
       (prset=dbGetRset(&self->addr)) &&
       prset->get_array_info)
    {
        char *datasave=self->addr.pfield;
        long noe, off; /* ignored */
        prset->get_array_info(&self->addr, &noe, &off);
        data = self->addr.pfield;
        /* get_array_info can modify pfield in >3.15.0.1 */
        self->addr.pfield = datasave;

    } else
        data = self->addr.pfield;

    if(self->addr.field_type>DBF_MENU) {
        PyErr_SetString(PyExc_TypeError, "Can not map field type to numpy type");
        return NULL;
    } else if(self->addr.field_type==DBF_STRING)
        dims[0] *= self->addr.field_size;

    desc = dbf2np[self->addr.field_type];
    Py_XINCREF(desc);
    return PyArray_NewFromDescr(&PyArray_Type, desc, 1, dims, NULL, data, flags, (PyObject*)self);

#else
    PyErr_SetNone(PyExc_NotImplementedError);
    return NULL;
#endif
}

static PyObject *pyField_getlen(pyField *self)
{
    rset *prset;

    if(self->addr.special==SPC_DBADDR &&
       (prset=dbGetRset(&self->addr)) &&
       prset->get_array_info)
    {
        char *datasave=self->addr.pfield;
        long noe, off;
        prset->get_array_info(&self->addr, &noe, &off);
        /* get_array_info can modify pfield in >3.15.0.1 */
        self->addr.pfield = datasave;
        return PyInt_FromLong(noe);

    } else
        return PyInt_FromLong(1);
}

static PyObject *pyField_setlen(pyField *self, PyObject *args)
{
    rset *prset = dbGetRset(&self->addr);
    Py_ssize_t len;

    if(!PyArg_ParseTuple(args, "n", &len))
        return NULL;

    if(self->addr.special!=SPC_DBADDR ||
       !prset ||
       !prset->put_array_info)
    {
        PyErr_SetString(PyExc_TypeError, "Not an array field");
        return NULL;
    }

    if(len<1 || len > self->addr.no_elements) {
        PyErr_Format(PyExc_ValueError, "Requested length %ld out of range [1,%lu)",
                        (long)len, (unsigned long)self->addr.no_elements);
        return NULL;
    }

    prset->put_array_info(&self->addr, len);

    Py_RETURN_NONE;
}

static PyObject* pyField_getTime(pyField *self)
{
    DBLINK *plink = self->addr.pfield;
    epicsTimeStamp ret;
    long sts, sec, nsec;

    if(self->addr.field_type!=DBF_INLINK) {
        PyErr_SetString(PyExc_TypeError, "getTime can only be used on DBF_INLINK fields");
        return NULL;
    }

    sts = dbGetTimeStamp(plink, &ret);
    if(sts) {
        PyErr_SetString(PyExc_TypeError, "getTime failed");
        return NULL;
    }

    sec = ret.secPastEpoch + POSIX_TIME_AT_EPICS_EPOCH;
    nsec = ret.nsec;

    return Py_BuildValue("ll", sec, nsec);
}

static PyObject* pyField_getAlarm(pyField *self)
{
    DBLINK *plink = self->addr.pfield;
    long sts;
    epicsEnum16 stat, sevr;

    if(self->addr.field_type!=DBF_INLINK) {
        PyErr_SetString(PyExc_TypeError, "getAlarm can only be used on DBF_INLINK fields");
        return NULL;
    }

    sts = dbGetAlarm(plink, &stat, &sevr);
    if(sts) {
        PyErr_SetString(PyExc_TypeError, "getAlarm failed");
        return NULL;
    }

    return Py_BuildValue("ll", (long)sevr, (long)stat);
}

static PyObject *pyField_len(pyField *self)
{
    return PyInt_FromLong(self->addr.no_elements);
}

static PyMethodDef pyField_methods[] = {
    {"name", (PyCFunction)pyField_name, METH_NOARGS,
     "Return Names (\"record\",\"field\")"},
    {"fieldinfo", (PyCFunction)pyField_fldinfo, METH_NOARGS,
     "Field type info\nReturn (type, size, #elements"},
    {"getval", (PyCFunction)pyField_getval, METH_NOARGS,
     "Returns scalar version of field value"},
    {"putval", (PyCFunction)pyField_putval, METH_VARARGS,
     "Sets field value from a scalar"},
    {"getarray", (PyCFunction)pyField_getarray, METH_NOARGS,
     "Return a numpy ndarray refering to this field for in-place operations."},
    {"getarraylen", (PyCFunction)pyField_getlen, METH_NOARGS,
     "Return current number of valid elements for array fields."},
    {"putarraylen", (PyCFunction)pyField_setlen, METH_VARARGS,
     "Set number of valid elements for array fields."},
    {"getTime", (PyCFunction)pyField_getTime, METH_NOARGS,
     "Return link target timestamp as a tuple (sec, nsec)."},
    {"getAlarm", (PyCFunction)pyField_getAlarm, METH_NOARGS,
     "Return link target alarm condtions as a tuple (severity, status)."},
    {"__len__", (PyCFunction)pyField_len, METH_NOARGS,
     "Maximum number of elements storable in this field"},
    {NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION < 3
static Py_ssize_t pyField_buf_getcount(pyField *self, Py_ssize_t *totallen)
{
    if(totallen)
        *totallen = self->addr.field_size * self->addr.no_elements;
    return 1;
}

static Py_ssize_t pyField_buf_getbuf(pyField *self, Py_ssize_t bufid, void **data)
{
    if(bufid!=0) {
        PyErr_SetString(PyExc_SystemError, "Requested invalid segment");
        return -1;
    }
    *data = self->addr.pfield;
    return self->addr.field_size * self->addr.no_elements;
}

static Py_ssize_t pyField_buf_getcharbuf(pyField *self, Py_ssize_t bufid, char **data)
{
    if(bufid!=0) {
        PyErr_SetString(PyExc_SystemError, "Requested invalid segment");
        return -1;
    } else if(self->addr.field_size!=1) {
        PyErr_SetString(PyExc_TypeError, "Field type must be CHAR or UCHAR");
        return -1;
    }
    *data = self->addr.pfield;
    return self->addr.field_size * self->addr.no_elements;
}
#endif

static int pyField_buf_getbufferproc(pyField *self, Py_buffer *buf, int flags)
{
    return PyBuffer_FillInfo(buf, (PyObject*)self,
                             self->addr.pfield,
                             self->addr.field_size * self->addr.no_elements,
                             0, flags);
}

static PyBufferProcs pyField_buf_methods = {
#if PY_MAJOR_VERSION < 3
    (readbufferproc)pyField_buf_getbuf,
    (writebufferproc)pyField_buf_getbuf,
    (segcountproc)pyField_buf_getcount,
    (charbufferproc)pyField_buf_getcharbuf,
#endif
    (getbufferproc)pyField_buf_getbufferproc,
    (releasebufferproc)NULL,
};


static PyTypeObject pyField_type = {
#if PY_MAJOR_VERSION >= 3
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,
#endif
    "_dbapi._Field",
    sizeof(pyField),
};

int pyField_prepare(PyObject *module)
{
    size_t i;

    import_array1(-1);

    pyField_type.tp_flags = Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE;
#if PY_MAJOR_VERSION < 3
    pyField_type.tp_flags |= Py_TPFLAGS_HAVE_GETCHARBUFFER|Py_TPFLAGS_HAVE_NEWBUFFER;
#endif
    pyField_type.tp_methods = pyField_methods;
    pyField_type.tp_as_buffer = &pyField_buf_methods;
    pyField_type.tp_init = (initproc)pyField_Init;

    pyField_type.tp_new = PyType_GenericNew;
    if(PyType_Ready(&pyField_type)<0)
        return -1;

    PyObject *typeobj=(PyObject*)&pyField_type;
    Py_INCREF(typeobj);
    if(PyModule_AddObject(module, "_Field", typeobj)) {
        Py_DECREF(typeobj);
        return -1;
    }

#ifdef HAVE_NUMPY
    for(i=0; i<=DBF_MENU; i++) {
        dbf2np[i] = PyArray_DescrFromType(dbf2np_map[i]);
        assert(dbf2np[i]);
    }
#endif

    return 0;
}

void pyField_cleanup(void)
{
    size_t i;

    for(i=0; i<=DBF_MENU; i++) {
        Py_XDECREF(dbf2np[i]);
        dbf2np[i] = NULL;
    }
}

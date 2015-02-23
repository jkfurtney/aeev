#include "Python.h"
#include "numpy/noprefix.h"
#include "stdio.h"
#include "ops.h"

double eval_double(PyObject *cell, int index)
{
    int op_code = PyInt_AS_LONG(PyTuple_GET_ITEM(cell,0));
    switch (op_code){

    case A_A_ADD: case A_S_ADD: case S_A_ADD: case S_S_ADD:
        return eval_double(PyTuple_GET_ITEM(cell, 1), index) +
               eval_double(PyTuple_GET_ITEM(cell, 2), index);

    case A_A_SUB: case A_S_SUB: case S_A_SUB: case S_S_SUB:
        return eval_double(PyTuple_GET_ITEM(cell, 1), index) -
               eval_double(PyTuple_GET_ITEM(cell, 2), index);

    case A_A_MUL: case A_S_MUL: case S_A_MUL: case S_S_MUL:
        return eval_double(PyTuple_GET_ITEM(cell, 1), index) *
               eval_double(PyTuple_GET_ITEM(cell, 2), index);

    case A_A_DIV: case A_S_DIV: case S_A_DIV: case S_S_DIV:
        return eval_double(PyTuple_GET_ITEM(cell, 1), index) /
               eval_double(PyTuple_GET_ITEM(cell, 2), index);

    case A_A_POW: case A_S_POW: case S_A_POW: case S_S_POW:
        return pow(eval_double(PyTuple_GET_ITEM(cell, 1), index),
                   eval_double(PyTuple_GET_ITEM(cell, 2), index));

    case A_NEGATE: case S_NEGATE:
        return -eval_double(PyTuple_GET_ITEM(cell, 1), index);

    case I_SCALAR:
        return PyFloat_AS_DOUBLE(PyTuple_GET_ITEM(cell, 1));

    case IA_SCALAR:
        return ((double *)((PyArrayObject *)
                           PyTuple_GET_ITEM(cell, 1))->data)[index];

    default:
        printf("got %i \n", op_code);
        PyErr_SetString(PyExc_ValueError, "unknown opcode");
        return 0.0;
    }
}

static PyObject *eval(PyObject *self, PyObject *args)
{
    // input is a tuple (opcode)
    PyObject *cell=0;
    if (!PyArg_ParseTuple(args, "O", &cell))
        return NULL;
    if (!PyTuple_Check(cell)) {
        PyErr_SetString(PyExc_ValueError,
                        "expected tuple");
        return NULL;
    }
    return PyFloat_FromDouble(eval_double(cell, 0));
}

#define CHUNK_SIZE 256

static PyObject *array_eval(PyObject *self, PyObject *args)
{
    // input is a tuple (opcode)
    PyObject *cell=0;
    PyObject *target=0;
    int size=0;
    int i=0;
    PyArrayObject *ar;

    if (!PyArg_ParseTuple(args, "OO", &cell, &target))
        return NULL;
    ar = (PyArrayObject *)PyArray_FROMANY(target,
                                          PyArray_DOUBLE,
                                          1,
                                          2,
                                          NPY_IN_ARRAY);
    if (! ar) {
        PyErr_SetString(PyExc_ValueError, "target array error");
        return NULL;
    }
    if (!PyTuple_Check(cell)) {
        PyErr_SetString(PyExc_ValueError, "expected tuple");
        return NULL;
    }
    size = PyArray_DIM(ar,0);
    for (i=0; i<size; i++){
        ((double *)ar->data)[i] = eval_double(cell, i);
    }
    Py_RETURN_NONE;
}

static PyObject *array_vm_eval(PyObject *self, PyObject *args)
{
    PyObject *opcodes=0;
    PyObject *double_literals=0;
    PyObject *array_literals=0;
    PyObject *target=0;
    double *c_double_literals=0;
    int alstack[16];
    double astack[8][CHUNK_SIZE];
    double dstack[16];
    int dstack_ptr=0;
    int astack_ptr=0;
    int alstack_ptr=0;
    int nops=0;
    int i,j,k;
    long *c_opcodes=0;
    double *c_target=0;
    int array_size=0;
    int outside_loops=0;
    if (!PyArg_ParseTuple(args, "OOOO", &opcodes, &double_literals,
                          &array_literals, &target)) return NULL;
    if ( (! PyArray_CheckExact(opcodes)) ||
         (! PyArray_ISINTEGER((PyArrayObject *)opcodes))  ||
         (! PyArray_IS_C_CONTIGUOUS((PyArrayObject *)opcodes)) ||
         (! PyArray_NDIM((PyArrayObject *)opcodes) == 1)) {
        PyErr_SetString(PyExc_ValueError, "opcodes should be 1d contiguous array of type int");
        return NULL;
    }
    nops = PyArray_DIM((PyArrayObject *)opcodes, 0);
    c_opcodes = (long *)((PyArrayObject *)opcodes)->data;

    if ( (! PyArray_CheckExact(double_literals)) ||
         (! PyArray_ISFLOAT((PyArrayObject *)double_literals))  ||
         (! PyArray_IS_C_CONTIGUOUS((PyArrayObject *)double_literals)) ||
         (! PyArray_NDIM((PyArrayObject *)double_literals) == 1)) {
        PyErr_SetString(PyExc_ValueError, "double_literals should be 1d contiguous array of type float");
        return NULL;
    }

    if ( (! PyArray_CheckExact(target)) ||
         (! PyArray_ISFLOAT((PyArrayObject *)target))  ||
         (! PyArray_IS_C_CONTIGUOUS((PyArrayObject *)target)) ||
         (! PyArray_NDIM((PyArrayObject *)target) == 1)) {
        PyErr_SetString(PyExc_ValueError, "target should be 1d contiguous array of type float");
        return NULL;
    }
    array_size = PyArray_DIM((PyArrayObject *)target, 0);
    c_target = PyArray_DATA((PyArrayObject *)target);

    if (array_size % CHUNK_SIZE) {
        PyErr_SetString(PyExc_ValueError, "for now arrays must be a multiple of the chunk size.");
        return NULL;
    }
    outside_loops = array_size/CHUNK_SIZE;

    if (!PyTuple_Check(array_literals)) {
        PyErr_SetString(PyExc_ValueError, "array_literals should be a tuple of contiguous arrays of type float, all the same shape as target.");
        return NULL;
    }

    c_double_literals = (double *)PyArray_DATA((PyArrayObject *) double_literals);
    // outer loop / i
    for (j=0; j<nops; j++) {
        int result_target = 0;
        int left_heap = 0;
        int right_heap = 0;
        long op = c_opcodes[j];

        if (op & SCALAR_BIT) {
            printf("literal load\n");
            dstack[dstack_ptr] = c_double_literals[op & ~SCALAR_BIT];
            dstack_ptr++;
        }
        else if (op & ARRAY_SCALAR_BIT) {
            printf("array load\n");
            alstack[alstack_ptr] = op & ~ARRAY_SCALAR_BIT;
            alstack_ptr++;
        }
        else  // normal op
        {
            double *res=0;
            double *a = 0;
            double *b = 0;

            if (op & RESULT_TO_TARGET) {
                result_target = 1;
                res = target;
            }
            else {
                res = astack[astack_ptr];
            }
            if (op & RIGHT_ON_HEAP) {
                right_heap = 1;
            }
            if (op & LEFT_ON_HEAP) {
                left_heap = 1;
            }
            op &= ~BYTECODE_MASK;

            printf("opcode %l %i \n", op, c_opcodes[j]);
            switch (op) {
            case A_A_ADD: {
                // establish where data is coming from and going to.
                /* if (result_target) res = target; */
                /* else res = astack[astack_ptr]; */

                /* if (left_heap) { */
                /*     a = (double *) */
                /*         PyArray_DATA((PyArrayObject *) */
                /*                      PyTuple_GET_ITEM( */
                /*                          array_literals, */
                /*                          alstack[alstack_ptr])); */
                /* } */
                /* else a = astack */

                // a and b are pointers into stack or heap memory where this
                // chunk starts.
                /* for (k=0; k<chunk_size; k++) { */
                /*     res[k] = a[k] + b[k]; */
                /* } */
                break;
            }

            }

        }

        //printf("opcode %i\n", c_opcodes[j]);
    }




    //target[i] = left_operand[j] + right_operand[k];
    Py_RETURN_NONE;
}

static PyObject *vm_eval(PyObject *self, PyObject *args)
{
    // input is a tuple (opcode)
    PyObject *ops=0;
    PyObject *literals=0;
    int c_op=0;
    int n_opt=0;
    int stack_pointer = 0; // points to available stack location
    double stack[32];
    int i;

    if (!PyArg_ParseTuple(args, "OO", &ops, &literals))
        return NULL;
    if (! (PyTuple_Check(ops) && PyTuple_Check(literals)))
        return NULL;
    n_opt = PyTuple_GET_SIZE(ops);
    for (i=0; i<n_opt; i++) {
        c_op = PyInt_AS_LONG(PyTuple_GET_ITEM(ops, i));
        if (c_op & SCALAR_BIT) {
            stack[stack_pointer] = PyFloat_AS_DOUBLE(
                PyTuple_GET_ITEM(literals, c_op & ~SCALAR_BIT));
            stack_pointer++;
        } else {
            switch (c_op) {
            case A_A_ADD: case A_S_ADD: case S_A_ADD: case S_S_ADD:
                stack[stack_pointer-2] = stack[stack_pointer-2] +
                                         stack[stack_pointer-1];
                stack_pointer--;
                break;
            case A_A_POW: case A_S_POW: case S_A_POW: case S_S_POW:
                stack[stack_pointer-2] = pow(stack[stack_pointer-2],
                                             stack[stack_pointer-1]);
                stack_pointer--;
                break;
            default:
                PyErr_SetString(PyExc_ValueError,
                                "unknown opcode");
                return NULL;
            }
        }
    }
    return PyFloat_FromDouble(stack[0]);
}



static PyObject *call_test_chunk(PyObject *self, PyObject *args)
{
    int size=0;
    int i=0;
    int j=0;
    PyArrayObject *ar0;
    PyArrayObject *ar1;
    PyArrayObject *ar2;

    double *data0;
    double *data1;
    double *data2;

    double local_a[CHUNK_SIZE];
    double local_b[CHUNK_SIZE];
    double local_c[CHUNK_SIZE];

    PyObject *par = PyTuple_GetItem(args, 0);
    ar0 = (PyArrayObject *)PyArray_FROMANY(par, PyArray_DOUBLE, 1, 2, NPY_IN_ARRAY);
    par = PyTuple_GetItem(args, 1);
    ar1 = (PyArrayObject *)PyArray_FROMANY(par, PyArray_DOUBLE, 1, 2, NPY_IN_ARRAY);
    par = PyTuple_GetItem(args, 2);
    ar2 = (PyArrayObject *)PyArray_FROMANY(par, PyArray_DOUBLE, 1, 2, NPY_IN_ARRAY);

    data0 = (double *)ar0->data;
    data1 = (double *)ar1->data;
    data2 = (double *)ar2->data;

    size = PyArray_DIM(ar0,0);
    int nsteps = size/CHUNK_SIZE;

    for (i=0; i<nsteps; i++) {
        for (j=0; j<CHUNK_SIZE; j++) { local_a[j] = data0[i*CHUNK_SIZE + j]; }
        for (j=0; j<CHUNK_SIZE; j++) { local_b[j] = data1[i*CHUNK_SIZE + j]; }
        for (j=0; j<CHUNK_SIZE; j++) { local_c[j] = local_a[j] + local_b[j]; }
        for (j=0; j<CHUNK_SIZE; j++) { data2[i+CHUNK_SIZE + j] = local_c[j]; }
    }

    Py_RETURN_NONE;
}

static PyObject *call_test(PyObject *self, PyObject *args)
{
    int size=0;
    int i=0;
    PyArrayObject *ar0;
    PyArrayObject *ar1;
    PyArrayObject *ar2;

    double *data0;
    double *data1;
    double *data2;


    PyObject *par = PyTuple_GetItem(args, 0);
    ar0 = (PyArrayObject *)PyArray_FROMANY(par, PyArray_DOUBLE, 1, 2, NPY_IN_ARRAY);
    par = PyTuple_GetItem(args, 1);
    ar1 = (PyArrayObject *)PyArray_FROMANY(par, PyArray_DOUBLE, 1, 2, NPY_IN_ARRAY);
    par = PyTuple_GetItem(args, 2);
    ar2 = (PyArrayObject *)PyArray_FROMANY(par, PyArray_DOUBLE, 1, 2, NPY_IN_ARRAY);

    data0 = (double *)ar0->data;
    data1 = (double *)ar1->data;
    data2 = (double *)ar2->data;

    size = PyArray_DIM(ar0,0);
    for (i=0; i<size; i++) {
        data2[i] = data0[i] + data1[i];
    }

    Py_RETURN_NONE;
}


// Module functions table.
static PyMethodDef
module_functions[] = {
    { "eval", eval, METH_VARARGS, "Say hello." },
    { "array_eval", array_eval, METH_VARARGS, "Say hello." },
    { "vm_eval", vm_eval, METH_VARARGS, "Say hello." },
    { "array_vm_eval", array_vm_eval, METH_VARARGS, "Say hello." },
    { "call_test", call_test, METH_VARARGS, "Say hello." },
    { "call_test_chunk", call_test_chunk, METH_VARARGS, "Say hello." },
    { NULL }
};

// This function is called to initialize the module.

void
initaeev(void)
{
    Py_InitModule3("aeev", module_functions, "A minimal module.");
    import_array();
}

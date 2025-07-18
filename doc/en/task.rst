Task APIs
*********

Task
----

The `Task` component provides a cross-platform API around `std::thread` with
some additional configuration of stack size, priority, and core affinity for
FreeRTOS / ESP, as well as providing a callback API which enables interruptible
sleeps and termination of the task.

It also supports firing off syncrhonous (blocking) and asynchronous
(non-blocking) functions in separate threads, with the option of configuring the
core id and other esp-specific paramters.

Code examples for the task API are provided in the `task` example folder.

.. ------------------------------- Example -------------------------------------

.. toctree::

   task_example

.. ---------------------------- API Reference ----------------------------------

API Reference
-------------

.. include-build-file:: inc/task.inc
.. include-build-file:: inc/run_on_core.inc

# -*- coding: utf-8 -*-

# FOGLAMP_BEGIN
# See: http://foglamp.readthedocs.io/
# FOGLAMP_END

"""FogLAMP Scheduler module"""

import time
import datetime
import math
import asyncio
import collections
import uuid
import logging  # TODO: Delete me
import sys  # TODO: Needed for logging delete me
from enum import IntEnum

import aiopg.sa
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql as pg_types

__author__ = "Terris Linenbach"
__copyright__ = "Copyright (c) 2017 OSIsoft, LLC"
__license__ = "Apache 2.0"
__version__ = "${VERSION}"


class Scheduler(object):
    """FogLAMP Task Scheduler
    
    Starts and manages subprocesses (called tasks) via entities
    called Schedules (when to execute) and ProcessSchedules (what to execute).
    
    Most methods are coroutines.
    """

    # TODO: Methods that accept a schedule and look in _schedule_execution
    # should accept _schedule_execution to avoid the lookup or just
    # accept _schedule_execution if a _Schedule reference is added to
    # it (requires converting _Schedule to class)

    # TODO: Change _process_scripts to _processes containing
    # _Process objects. Then add process reference to _Schedule
    # to avoid script lookup.

    class _ScheduleType(IntEnum):
        """Enumeration for schedules.schedule_type"""
        TIMED = 1
        INTERVAL = 2
        MANUAL = 3
        STARTUP = 4

    class _TaskState(IntEnum):
        """Enumeration for tasks.task_state"""
        RUNNING = 1
        COMPLETE = 2
        CANCELED = 3
        INTERRUPTED = 4

    _Schedule = collections.namedtuple(
        'Schedule',
        'id name type time day interval repeat_seconds exclusive process_name')
    """Represents a row in the schedules table"""

    class _ScheduleExecution:
        """Tracks information about schedules"""
        __slots__ = ['next_start_time', 'task_processes']

        def __init__(self):
            self.next_start_time = None
            """When to next start a task for the schedule"""
            self.task_processes = dict()
            """Maps a task id to a process"""

    # Class attributes (begin)
    __scheduled_processes_tbl = None  # type: sa.Table
    __schedules_tbl = None  # type: sa.Table
    __tasks_tbl = None  # type: sa.Table

    # Constants:
    __CONNECTION_STRING = "dbname='foglamp'"
    # "postgresql://foglamp:foglamp@localhost:5432/foglamp"
    __HOUR_SECONDS = 3600
    __DAY_SECONDS = 3600*24
    __WEEK_SECONDS = 3600*24*7
    __MAX_SLEEP = 9999999
    __ONE_HOUR = datetime.timedelta(hours=1)
    __ONE_DAY = datetime.timedelta(days=1)
    """When there is nothing to do, sleep for this number of seconds (forever)"""
    # Class attributes (end)

    def __init__(self):
        # Class variables (begin)
        if self.__schedules_tbl is None:
            self.__schedules_tbl = sa.Table(
                'schedules',
                sa.MetaData(),
                sa.Column('id', pg_types.UUID),
                sa.Column('schedule_name', sa.types.VARCHAR(20)),
                sa.Column('process_name', sa.types.VARCHAR(20)),
                sa.Column('schedule_type', sa.types.SMALLINT),
                sa.Column('schedule_time', sa.types.TIME),
                sa.Column('schedule_day', sa.types.SMALLINT),
                sa.Column('schedule_interval', sa.types.Interval),
                sa.Column('exclusive', sa.types.BOOLEAN))

            self.__tasks_tbl = sa.Table(
                'tasks',
                sa.MetaData(),
                sa.Column('id', pg_types.UUID),
                sa.Column('process_name', sa.types.VARCHAR(20)),
                sa.Column('state', sa.types.INT),
                sa.Column('start_time', sa.types.TIMESTAMP),
                sa.Column('end_time', sa.types.TIMESTAMP),
                sa.Column('pid', sa.types.INT),
                sa.Column('exit_code', sa.types.INT),
                sa.Column('reason', sa.types.VARCHAR(255)))

            self.__scheduled_processes_tbl = sa.Table(
                'scheduled_processes',
                sa.MetaData(),
                sa.Column('name', pg_types.VARCHAR(20)),
                sa.Column('script', pg_types.JSONB))
        # Class variables (end)

        # Instance variables (begin)
        self._start_time = None
        """When the scheduler started"""
        self._paused = False
        """When True, the scheduler will not start any new tasks"""
        self._process_scripts = dict()
        """Dictionary of scheduled_processes.id to script. Immutable."""
        self._schedules = dict()
        """Dictionary of schedules.id to _Schedule"""
        self._schedule_executions = dict()
        """Dictionary of schedules.id to _ScheduleExecution"""
        self._active_task_count = 0
        """Number of active tasks"""
        self._main_sleep_task = None
        """Coroutine that sleeps in the main loop"""
        # Instance variables (end)

    async def stop(self):
        """Attempts to stop the scheduler

        Sends TERM signal to all running tasks. Does not wait for tasks to stop.

        Prevents any new tasks from starting. This can be undone by setting the
        _paused attribute to False.

        Raises TimeoutError:
            A task is still running. Wait and try again.
        """
        logging.getLogger(__name__).info("Stop requested")

        """Stop the main loop"""
        self._paused = True
        self._resume_check_schedules()

        # Can not iterate over _schedule_executions - it can change mid-iteration
        for schedule_id in list(self._schedule_executions.keys()):
            try:
                schedule_execution = self._schedule_executions[schedule_id]
            except KeyError:
                continue

            for task_id in list(schedule_execution.task_processes):
                try:
                    process = schedule_execution.task_processes[task_id]
                except KeyError:
                    continue

                # TODO: The schedule might disappear
                #       This problem is rampant in the code base for
                #       _schedules and _scheduled_processes
                schedule = self._schedules[schedule_id]

                logging.getLogger(__name__).info(
                    "Terminating: Schedule '%s' process '%s' task %s pid %s\n%s",
                    schedule.name,
                    schedule.process_name,
                    task_id,
                    process.pid,
                    self._process_scripts[schedule.process_name])

                try:
                    process.terminate()
                except ProcessLookupError:
                    pass  # Process has terminated

        await asyncio.sleep(.1)  # sleep 0 doesn't give the process enough time to quit

        if self._active_task_count:
            raise TimeoutError()

        logging.getLogger(__name__).info("Stopped")
        self._start_time = None

        return True

    async def _start_task(self, schedule):
        task_id = str(uuid.uuid4())
        args = self._process_scripts[schedule.process_name]
        logging.getLogger(__name__).info("Starting: Schedule '%s' process '%s' task %s\n%s",
                                         schedule.name, schedule.process_name, task_id, args)

        process = None

        try:
            process = await asyncio.create_subprocess_exec(*args)
        except Exception:
            # TODO: catch real exception
            logging.getLogger(__name__).exception(
                "Unable to start schedule '%s' process '%s' task %s\n%s".format(
                    schedule.name, schedule.process_name, task_id, args))

        if process:
            logging.getLogger(__name__).info(
                "Started: Schedule '%s' process '%s' task %s pid %s\n%s",
                schedule.name, schedule.process_name, task_id, process.pid, args)

            self._schedule_executions[schedule.id].task_processes[task_id] = process
        else:
            self._active_task_count -= 1

        return task_id

    async def _start_startup_task(self, schedule):
        """Startup tasks are not tracked in the tasks table"""
        # TODO: what if this fails?
        task_id = await self._start_task(schedule)
        asyncio.ensure_future(self._wait_for_startup_task_completion(schedule, task_id))

    async def _start_regular_task(self, schedule):
        task_id = await self._start_task(schedule)

        """The task row needs to exist before the completion handler runs"""
        async with aiopg.sa.create_engine(self.__CONNECTION_STRING) as engine:
            async with engine.acquire() as conn:
                await conn.execute(self.__tasks_tbl.insert().values(
                            id=task_id,
                            pid=self._schedule_executions[schedule.id].task_processes[task_id].pid,
                            process_name=schedule.process_name,
                            state=int(self._TaskState.RUNNING),
                            start_time=datetime.datetime.now()))

        asyncio.ensure_future(self._wait_for_task_completion(schedule, task_id))

    def _on_task_completion(self, schedule, process, task_id):
        logging.getLogger(__name__).info(
            "Exited: Schedule '%s' process '%s' task %s pid %s\n%s",
            schedule.name,
            schedule.process_name,
            task_id,
            process.pid,
            self._process_scripts[schedule.process_name])

        if self._active_task_count:
            self._active_task_count -= 1
        else:
            """This should not happen!"""
            logging.getLogger(__name__).error("Active task count would be negative")

        schedule_execution = self._schedule_executions[schedule.id]

        if schedule.exclusive and self._schedule_next_task(schedule):
            self._resume_check_schedules()

        if schedule_execution.next_start_time is None:
            del self._schedule_executions[schedule.id]
        else:
            del schedule_execution.task_processes[task_id]

    async def _wait_for_startup_task_completion(self, schedule, task_id):
        # TODO: Restart if the process terminates unexpectedly
        process = self._schedule_executions[schedule.id].task_processes[task_id]

        try:
            await process.wait()
        except Exception:  # TODO: catch real exception
            pass

        self._on_task_completion(schedule, process, task_id)

    async def _wait_for_task_completion(self, schedule, task_id):
        process = self._schedule_executions[schedule.id].task_processes[task_id]

        exit_code = None
        try:
            exit_code = await process.wait()
        except Exception:  # TODO: catch real exception
            pass

        self._on_task_completion(schedule, process, task_id)

        """Update the task's status"""
        async with aiopg.sa.create_engine(self.__CONNECTION_STRING) as engine:
            async with engine.acquire() as conn:
                await conn.execute(
                    self.__tasks_tbl.update().
                    where(self.__tasks_tbl.c.id == task_id).
                    values(exit_code=exit_code,
                           state=int(self._TaskState.COMPLETE),
                           end_time=datetime.datetime.now()))

    async def _check_schedules(self):
        """Starts tasks according to schedules based on the current time"""
        least_next_start_time = None

        """Can not iterate over _next_starts - it can change mid-iteration"""
        for key in list(self._schedule_executions.keys()):
            if self._paused:
                return None

            try:
                schedule = self._schedules[key]
            except KeyError:
                continue

            schedule_execution = self._schedule_executions[key]

            if schedule.exclusive and schedule_execution.task_processes:
                continue

            next_start_time = schedule_execution.next_start_time

            if not next_start_time:
                continue

            if time.time() >= schedule_execution.next_start_time:
                """"It's time to create a task for this schedule.
                
                Because there are so many 'awaits' in this block,
                the active task count is incremented prior to any
                'await' calls. Otherwise, a stop() request
                would terminate before the process gets
                started and tracked. Thus the code
                following this increment and all code that is
                called must be very careful about exceptions
                so that _active_task_count will decremented if
                necessary.
                """
                self._active_task_count += 1

                """Modify next_start_time immediately to avoid reentrancy bugs"""
                if not schedule.exclusive and self._schedule_next_task(schedule):
                    next_start_time = schedule_execution.next_start_time
                else:
                    next_start_time = None

                if schedule.type == self._ScheduleType.STARTUP:
                    await self._start_startup_task(schedule)
                else:
                    await self._start_regular_task(schedule)

            """Track the least next_start_time"""
            if next_start_time is not None and (least_next_start_time is None
                                                or least_next_start_time > next_start_time):
                least_next_start_time = next_start_time

        return least_next_start_time

    def _schedule_next_timed_task(self, schedule, schedule_execution, current_dt):
        """Handle daylight savings time transitions"""
        if schedule.repeat_seconds == self.__HOUR_SECONDS:
            """If hourly repeat, use the current hour. Ignore the specified hour."""
            dt = datetime.datetime(
                year=current_dt.year,
                month=current_dt.month,
                day=current_dt.day,
                hour=current_dt.hour,
                minute=schedule.time.minute,
                second=schedule.time.second)

            if dt.time() > schedule.time:
                dt += self.__ONE_HOUR
        else:
            dt = datetime.datetime(
                year=current_dt.year,
                month=current_dt.month,
                day=current_dt.day,
                hour=schedule.time.hour,
                minute=schedule.time.minute,
                second=schedule.time.second)

            if current_dt.time() > schedule.time:
                dt += self.__ONE_DAY

        # Advance to the correct day if specified
        if schedule.day:
            while dt.isoweekday() != schedule.day:
                dt += self.__ONE_DAY

        schedule_execution.next_start_time = time.mktime(dt.timetuple())

    def _schedule_first_task(self, schedule, current_time):
        """Compute the first time a schedule should start

        Args:
            schedule _Schedule:
            current_time int:
        """
        schedule_execution = self._ScheduleExecution()
        self._schedule_executions[schedule.id] = schedule_execution

        if schedule.type == self._ScheduleType.INTERVAL:
            schedule_execution.next_start_time = current_time + schedule.repeat_seconds
        elif schedule.type == self._ScheduleType.TIMED:
            self._schedule_next_timed_task(
                schedule,
                schedule_execution,
                datetime.datetime.fromtimestamp(current_time))
        elif schedule.type == self._ScheduleType.STARTUP:
            schedule_execution.next_start_time = current_time

        logging.getLogger(__name__).info(
            "Scheduled '%s' for %s", schedule.name,
            datetime.datetime.fromtimestamp(schedule_execution.next_start_time))

    def _schedule_next_task(self, schedule):
        advance_seconds = schedule.repeat_seconds

        schedule_execution = self._schedule_executions[schedule.id]

        if self._paused or advance_seconds is None:
            schedule_execution.next_start_time = None
            return False

        if schedule.exclusive:
            """next_start_time is not updated for exclusive tasks by the main
               loop. This attribute is instead updated after the task finishes.
               Compute the number of intervals that passed during the
               task's execution.
            """
            if advance_seconds:
                advance_seconds *= math.ceil(
                    (time.time() - schedule_execution.next_start_time) /
                    advance_seconds)
            else:
                advance_seconds = time.time()-schedule_execution.next_start_time

        if schedule.type == self._ScheduleType.TIMED:
            """Handle daylight savings time transitions"""
            next_dt = datetime.datetime.fromtimestamp(schedule_execution.next_start_time)
            next_dt += datetime.timedelta(seconds=advance_seconds)

            if schedule.day is not None and next_dt.isoweekday() != schedule.day:
                """Advance to the next matching day"""
                next_dt = datetime.datetime(year=next_dt.year,
                                            month=next_dt.month,
                                            day=next_dt.day)
                self._schedule_next_timed_task(schedule, schedule_execution, next_dt)
            else:
                schedule_execution.next_start_time = time.mktime(next_dt.timetuple())
        else:
            schedule_execution.next_start_time += advance_seconds

        logging.getLogger(__name__).info(
            "Scheduled '%s' for %s", schedule.name,
            datetime.datetime.fromtimestamp(schedule_execution.next_start_time))

        return True

    async def _get_process_scripts(self):
        query = sa.select([self.__scheduled_processes_tbl.c.name,
                           self.__scheduled_processes_tbl.c.script])
        query.select_from(self.__scheduled_processes_tbl)

        async with aiopg.sa.create_engine(self.__CONNECTION_STRING) as engine:
            async with engine.acquire() as conn:
                async for row in conn.execute(query):
                    self._process_scripts[row.name] = row.script

    async def _get_schedules(self):
        # TODO: Get processes first, then add to Schedule
        query = sa.select([self.__schedules_tbl.c.id,
                           self.__schedules_tbl.c.schedule_name,
                           self.__schedules_tbl.c.schedule_type,
                           self.__schedules_tbl.c.schedule_time,
                           self.__schedules_tbl.c.schedule_day,
                           self.__schedules_tbl.c.schedule_interval,
                           self.__schedules_tbl.c.exclusive,
                           self.__schedules_tbl.c.process_name])

        query.select_from(self.__schedules_tbl)

        async with aiopg.sa.create_engine(self.__CONNECTION_STRING) as engine:
            async with engine.acquire() as conn:
                async for row in conn.execute(query):
                    interval = row.schedule_interval

                    repeat_seconds = None
                    if interval is not None:
                        repeat_seconds = interval.total_seconds()

                    schedule = self._Schedule(
                        id=row.id,
                        name=row.schedule_name,
                        type=row.schedule_type,
                        day=row.schedule_day,
                        time=row.schedule_time,
                        interval=interval,
                        repeat_seconds=repeat_seconds,
                        exclusive=row.exclusive,
                        process_name=row.process_name)

                    # TODO: Move this to _add_schedule to check for errors
                    self._schedules[row.id] = schedule
                    self._schedule_first_task(schedule, self._start_time)

    async def _read_storage(self):
        """Reads schedule information from the storage server"""
        await self._get_process_scripts()
        await self._get_schedules()

    def _resume_check_schedules(self):
        if self._main_sleep_task:
            self._main_sleep_task.cancel()

    async def _main(self):
        """Main loop for the scheduler

        - Reads configuration and schedules
        - Runs :meth:`Scheduler._check_schedules` in an endless loop
        """
        # TODO: log exception here or add an exception handler in asyncio
        await self._read_storage()

        while True:
            next_start_time = await self._check_schedules()

            if self._paused:
                break

            if next_start_time is None:
                sleep_seconds = self.__MAX_SLEEP
            else:
                sleep_seconds = next_start_time - time.time()

            logging.getLogger(__name__).info("Sleeping for %s seconds", sleep_seconds)

            self._main_sleep_task = asyncio.ensure_future(asyncio.sleep(sleep_seconds))

            try:
                await self._main_sleep_task
            except asyncio.CancelledError:
                logging.getLogger(__name__).debug("Main loop awakened")
                pass

            self._main_sleep_task = None

    # TODO: Used for development. Delete me.
    @staticmethod
    def _debug_logging_stdout():
        ch = logging.StreamHandler(sys.stdout)
        ch.setLevel(logging.DEBUG)
        formatter = logging.Formatter('%(asctime)s - %(name)s - %(levelname)s - %(message)s')
        ch.setFormatter(formatter)
        logging.getLogger().setLevel(logging.DEBUG)
        logging.getLogger().addHandler(ch)

    def start(self):
        """Starts the scheduler

        Returns with scheduler running in the background

        Raises RuntimeError:
            Scheduler already started
        """
        if self._start_time:
            raise RuntimeError("The scheduler is already running")

        self._start_time = time.time()

        self._debug_logging_stdout()
        logging.getLogger(__name__).info("Starting")

        asyncio.ensure_future(self._main())

        """Hard-code storage server
        wait self._start_startup_task(self._schedules['storage'])
        """

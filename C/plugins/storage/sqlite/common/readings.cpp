/*
 * Fledge storage service.
 *
 * Copyright (c) 2018 OSIsoft, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Massimiliano Pinto
 */

#include <math.h>
#include <connection.h>
#include <connection_manager.h>
#include <common.h>
#include <reading_stream.h>
#include <random>
#include <map>
#include <utils.h>
//# FIXME_I:

#include <sys/stat.h>
#include <libgen.h>
//#include <mutex>

// 1 enable performance tracking
#define INSTRUMENT	0


#if INSTRUMENT
#include <sys/time.h>
#endif

// Decode stream data
#define	RDS_USER_TIMESTAMP(stream, x) 	stream[x]->userTs
#define	RDS_ASSET_CODE(stream, x)		stream[x]->assetCode
#define	RDS_PAYLOAD(stream, x)			&(stream[x]->assetCode[0]) + stream[x]->assetCodeLength

// Retry mechanism
#define PREP_CMD_MAX_RETRIES		20	    // Maximum no. of retries when a lock is encountered
#define PREP_CMD_RETRY_BASE 		5000    // Base time to wait for
#define PREP_CMD_RETRY_BACKOFF		5000 	// Variable time to wait for

/*
 * Control the way purge deletes readings. The block size sets a limit as to how many rows
 * get deleted in each call, whilst the sleep interval controls how long the thread sleeps
 * between deletes. The idea is to not keep the database locked too long and allow other threads
 * to have access to the database between blocks.
 */
#define PURGE_SLEEP_MS 500
#define PURGE_DELETE_BLOCK_SIZE	20
#define TARGET_PURGE_BLOCK_DEL_TIME	(70*1000) 	// 70 msec
#define PURGE_BLOCK_SZ_GRANULARITY	5 	// 5 rows
#define MIN_PURGE_DELETE_BLOCK_SIZE	20
#define MAX_PURGE_DELETE_BLOCK_SIZE	1500
#define RECALC_PURGE_BLOCK_SIZE_NUM_BLOCKS	30	// recalculate purge block size after every 30 blocks

#define PURGE_SLOWDOWN_AFTER_BLOCKS 5
#define PURGE_SLOWDOWN_SLEEP_MS 500

#define SECONDS_PER_DAY "86400.0"
// 2440587.5 is the julian day at 1/1/1970 0:00 UTC.
#define JULIAN_DAY_START_UNIXTIME "2440587.5"

//#ifndef PLUGIN_LOG_NAME
//#define PLUGIN_LOG_NAME "SQLite 3"
//#endif

/**
 * SQLite3 storage plugin for Fledge
 */

using namespace std;
using namespace rapidjson;

#define CONNECT_ERROR_THRESHOLD		5*60	// 5 minutes

#define MAX_RETRIES			40	// Maximum no. of retries when a lock is encountered
#define RETRY_BACKOFF			100	// Multipler to backoff DB retry on lock

/*
 * The following allows for conditional inclusion of code that tracks the top queries
 * run by the storage plugin and the number of times a particular statement has to
 * be retried because of the database being busy./
 */
#define DO_PROFILE		0
#define DO_PROFILE_RETRIES	0
#if DO_PROFILE
#include <profile.h>

#define	TOP_N_STATEMENTS		10	// Number of statements to report in top n
#define RETRY_REPORT_THRESHOLD		1000	// Report retry statistics every X calls

QueryProfile profiler(TOP_N_STATEMENTS);
unsigned long retryStats[MAX_RETRIES] = { 0,0,0,0,0,0,0,0,0,0 };
unsigned long numStatements = 0;
int	      maxQueue = 0;
#endif

static std::atomic<int> m_waiting(0);
static std::atomic<int> m_writeAccessOngoing(0);
static std::mutex	db_mutex;
static std::condition_variable	db_cv;
static int purgeBlockSize = PURGE_DELETE_BLOCK_SIZE;


#define START_TIME std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
#define END_TIME std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now(); \
				 auto usecs = std::chrono::duration_cast<std::chrono::microseconds>( t2 - t1 ).count();

static time_t connectErrorTime = 0;


/**
 * Check whether to compute timebucket query with min,max,avg for all datapoints
 *
 * @param    payload	JSON payload
 * @return		True if aggregation is 'all'
 */
bool aggregateAll(const Value& payload)
{
	if (payload.HasMember("aggregate") &&
	    payload["aggregate"].IsObject())
	{
		const Value& agg = payload["aggregate"];
		if (agg.HasMember("operation") &&
		    (strcmp(agg["operation"].GetString(), "all") == 0))
		{
			return true;
		}
	}
	return false;
}

/**
 * Build, exucute and return data of a timebucket query with min,max,avg for all datapoints
 *
 * @param    payload	JSON object for timebucket query
 * @param    resultSet	JSON Output buffer
 * @return		True of success, false on any error
 */
bool Connection::aggregateQuery(const Value& payload, string& resultSet)
{
	if (!payload.HasMember("where") ||
	    !payload.HasMember("timebucket"))
	{
		raiseError("retrieve", "aggregateQuery is missing "
			   "'where' and/or 'timebucket' properties");
		return false;
	}

	SQLBuffer sql;

	sql.append("SELECT asset_code, ");

	double size = 1;
	string timeColumn;

	// Check timebucket object
	if (payload.HasMember("timebucket"))
	{
		const Value& bucket = payload["timebucket"];
		if (!bucket.HasMember("timestamp"))
		{
			raiseError("retrieve", "aggregateQuery is missing "
				   "'timestamp' property for 'timebucket'");
			return false;
		}

		// Time column
		timeColumn = bucket["timestamp"].GetString();

		// Bucket size
		if (bucket.HasMember("size"))
		{
			size = atof(bucket["size"].GetString());
			if (!size)
			{
				size = 1;
			}
		}

		// Time format for output
		string newFormat;
		if (bucket.HasMember("format") && size >= 1)
		{
			applyColumnDateFormatLocaltime(bucket["format"].GetString(),
							"timestamp",
							newFormat,
							true);
			sql.append(newFormat);
		}
		else
		{
			if (size < 1) 
			{
				// sub-second granularity to time bucket size:
				// force output formatting with microseconds	
				newFormat = "strftime('%Y-%m-%d %H:%M:%S', " + timeColumn + 
					    ", 'localtime') || substr(" + timeColumn	  +
					    ", instr(" + timeColumn + ", '.'), 7)";
				sql.append(newFormat);
			}
			else
			{
				sql.append("timestamp");
			}
		}

		// Time output alias
		if (bucket.HasMember("alias"))
		{
			sql.append(" AS ");
			sql.append(bucket["alias"].GetString());
		}
	}

	// JSON format aggregated data
	sql.append(", '{' || group_concat('\"' || x || '\" : ' || resd, ', ') || '}' AS reading ");

	// subquery
	sql.append("FROM ( SELECT  x, asset_code, max(timestamp) AS timestamp, ");
	// Add min
	sql.append("'{\"min\" : ' || min(theval) || ', ");
	// Add max
	sql.append("\"max\" : ' || max(theval) || ', ");
	// Add avg
	sql.append("\"average\" : ' || avg(theval) || ', ");
	// Add count
	sql.append("\"count\" : ' || count(theval) || ', ");
	// Add sum
	sql.append("\"sum\" : ' || sum(theval) || '}' AS resd ");

	if (size < 1)
	{
		// Add max(user_ts)
		sql.append(", max(" + timeColumn + ") AS " + timeColumn + " ");
	}

	// subquery
	sql.append("FROM ( SELECT asset_code, ");
	sql.append(timeColumn);

	if (size >= 1)
	{
		sql.append(", datetime(");
	}
	else
	{
		sql.append(", (");
	}

	// Size formatted string
	string size_format;
	if (fmod(size, 1.0) == 0.0)
	{
		size_format = to_string(int(size));
	}
	else
	{
		size_format = to_string(size);
	}

	// Add timebucket size
	// Unix Time is (Julian Day - JulianDay(1/1/1970 0:00 UTC) * Seconds_per_day
	if (size != 1)
	{
		sql.append(size_format);
		sql.append(" * round((julianday(");
		sql.append(timeColumn);
		sql.append(") - " + string(JULIAN_DAY_START_UNIXTIME) + ") * " + string(SECONDS_PER_DAY) + " / ");
		sql.append(size_format);
		sql.append(")");
	}
	else
	{
		sql.append("round((julianday(");
		sql.append(timeColumn);
		sql.append(") - " + string(JULIAN_DAY_START_UNIXTIME) + ") * " + string(SECONDS_PER_DAY) + " / 1)");
	}
	if (size >= 1)
	{
		sql.append(", 'unixepoch') AS \"timestamp\", reading, ");
	}
	else
	{
		sql.append(") AS \"timestamp\", reading, ");
	}

	// Get all datapoints in 'reading' field
	sql.append("json_each.key AS x, json_each.value AS theval FROM " DB_READINGS ".readings_1, json_each(readings.reading) ");

	// Add where condition
	sql.append("WHERE ");
	if (!jsonWhereClause(payload["where"], sql))
	{
		raiseError("retrieve", "aggregateQuery: failure while building WHERE clause");
		return false;
	}

	// close subquery
	sql.append(") tmp ");

	// Add group by
	// Unix Time is (Julian Day - JulianDay(1/1/1970 0:00 UTC) * Seconds_per_day
	sql.append(" GROUP BY x, asset_code, ");
	sql.append("round((julianday(");
	sql.append(timeColumn);
	sql.append(") - " + string(JULIAN_DAY_START_UNIXTIME) + ") * " + string(SECONDS_PER_DAY) + " / ");

	if (size != 1)
	{
		sql.append(size_format);
	}
	else
	{
		sql.append('1');
	}
	sql.append(") ");

	// close subquery
	sql.append(") tbl ");

	// Add final group and sort
	sql.append("GROUP BY timestamp, asset_code ORDER BY timestamp DESC");

	// Add limit
	if (payload.HasMember("limit"))
	{
		if (!payload["limit"].IsInt())
		{
			raiseError("retrieve", "aggregateQuery: limit must be specfied as an integer");
			return false;
		}
		sql.append(" LIMIT ");
		try {
			sql.append(payload["limit"].GetInt());
		} catch (exception e) {
			raiseError("retrieve", "aggregateQuery: bad value for limit parameter: %s", e.what());
			return false;
		}
	}
	sql.append(';');

	// Execute query
	const char *query = sql.coalesce();
	int rc;
	sqlite3_stmt *stmt;

	logSQL("CommonRetrieve", query);

	// Prepare the SQL statement and get the result set
	rc = sqlite3_prepare_v2(dbHandle, query, -1, &stmt, NULL);

	// Release memory for 'query' var
	delete[] query;

	if (rc != SQLITE_OK)
	{
		raiseError("retrieve", sqlite3_errmsg(dbHandle));
		return false;
	}

	// Call result set mapping
	rc = mapResultSet(stmt, resultSet);

	// Delete result set
	sqlite3_finalize(stmt);

	// Check result set mapping errors
	if (rc != SQLITE_DONE)
	{
		raiseError("retrieve", sqlite3_errmsg(dbHandle));
		// Failure
		return false;
	}

	return true;
}

/**
 * Append a stream of readings to SQLite db
 *
 * @param readings  readings to store into the SQLite db
 * @param commit    if true a database commit is executed and a new transaction will be opened at the next execution
 *
 */
int Connection::readingStream(ReadingStream **readings, bool commit)
{
	// Row defintion related
	int i;
	bool add_row = false;
	const char *user_ts;
	string now;
	char ts[60], micro_s[10];
	char formatted_date[LEN_BUFFER_DATE] = {0};
	struct tm timeinfo;
	const char *asset_code;
	const char *payload;
	string reading;

	// Retry mechanism
	int retries = 0;
	int sleep_time_ms = 0;

	// SQLite related
	sqlite3_stmt *stmt;
	int sqlite3_resut;
	int rowNumber = -1;

#if INSTRUMENT
	struct timeval start, t1, t2, t3, t4, t5;
#endif

	const char *sql_cmd = "INSERT INTO  " DB_READINGS ".readings_1 ( asset_code, reading, user_ts ) VALUES  (?,?,?)";

	if (sqlite3_prepare_v2(dbHandle, sql_cmd, strlen(sql_cmd), &stmt, NULL) != SQLITE_OK)
	{
		raiseError("readingStream", sqlite3_errmsg(dbHandle));
		return -1;
	}

	// The handling of the commit parameter is overridden as using a pool of connections every execution receives
	// a differen one, so a commit at every run is executed.
	m_streamOpenTransaction = true;
	commit = true;

	if (m_streamOpenTransaction)
	{
		if (sqlite3_exec(dbHandle, "BEGIN TRANSACTION", NULL, NULL, NULL) != SQLITE_OK)
		{
			raiseError("readingStream", sqlite3_errmsg(dbHandle));
			return -1;
		}
		m_streamOpenTransaction = false;
	}

#if INSTRUMENT
	gettimeofday(&start, NULL);
#endif

	try
	{
		for (i = 0; readings[i]; i++)
		{
			add_row = true;

			// Handles - asset_code
			asset_code = RDS_ASSET_CODE(readings, i);

			// Handles - reading
			payload = RDS_PAYLOAD(readings, i);
			reading = escape(payload);

			// Handles - user_ts
			memset(&timeinfo, 0, sizeof(struct tm));
			gmtime_r(&RDS_USER_TIMESTAMP(readings, i).tv_sec, &timeinfo);
			std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &timeinfo);
			snprintf(micro_s, sizeof(micro_s), ".%06lu", RDS_USER_TIMESTAMP(readings, i).tv_usec);

			formatted_date[0] = {0};
			strncat(ts, micro_s, 10);
			user_ts = ts;
			if (strcmp(user_ts, "now()") == 0)
			{
				getNow(now);
				user_ts = now.c_str();
			}
			else
			{
				if (!formatDate(formatted_date, sizeof(formatted_date), user_ts))
				{
					raiseError("appendReadings", "Invalid date |%s|", user_ts);
					add_row = false;
				}
				else
				{
					user_ts = formatted_date;
				}
			}

			if (add_row)
			{
				if (stmt != NULL)
				{
					sqlite3_bind_text(stmt, 1, asset_code,      -1, SQLITE_STATIC);
					sqlite3_bind_text(stmt, 2, reading.c_str(), -1, SQLITE_STATIC);
					sqlite3_bind_text(stmt, 3, user_ts,         -1, SQLITE_STATIC);

					retries =0;
					sleep_time_ms = 0;

					// Retry mechanism in case SQLlite DB is locked
					do {
						// Insert the row using a lock to ensure one insert at time
						{
							m_writeAccessOngoing.fetch_add(1);
							//unique_lock<mutex> lck(db_mutex);

							sqlite3_resut = sqlite3_step(stmt);

							m_writeAccessOngoing.fetch_sub(1);
							//db_cv.notify_all();
						}

						if (sqlite3_resut == SQLITE_LOCKED  )
						{
							sleep_time_ms = PREP_CMD_RETRY_BASE + (random() %  PREP_CMD_RETRY_BACKOFF);
							retries++;

							Logger::getLogger()->info("SQLITE_LOCKED - record :%d: - retry number :%d: sleep time ms :%d:",i, retries, sleep_time_ms);

							std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time_ms));
						}
						if (sqlite3_resut == SQLITE_BUSY)
						{
							ostringstream threadId;
							threadId << std::this_thread::get_id();

							sleep_time_ms = PREP_CMD_RETRY_BASE + (random() %  PREP_CMD_RETRY_BACKOFF);
							retries++;

							Logger::getLogger()->info("SQLITE_BUSY - thread :%s: - record :%d: - retry number :%d: sleep time ms :%d:", threadId.str().c_str() ,i , retries, sleep_time_ms);

							std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time_ms));
						}
					} while (retries < PREP_CMD_MAX_RETRIES && (sqlite3_resut == SQLITE_LOCKED || sqlite3_resut == SQLITE_BUSY));

					if (sqlite3_resut == SQLITE_DONE)
					{
						rowNumber++;

						sqlite3_clear_bindings(stmt);
						sqlite3_reset(stmt);
					}
					else
					{
						raiseError("appendReadings",
								   "Inserting a row into SQLIte using a prepared command - asset_code :%s: error :%s: reading :%s: ",
								   asset_code,
								   sqlite3_errmsg(dbHandle),
								   reading.c_str());

						sqlite3_exec(dbHandle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
						m_streamOpenTransaction = true;
						return -1;
					}
				}
			}
		}
		rowNumber = i;

	} catch (exception e) {

		raiseError("appendReadings", "Inserting a row into SQLIte using a prepared command - error :%s:", e.what());

		sqlite3_exec(dbHandle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
		m_streamOpenTransaction = true;
		return -1;
	}

#if INSTRUMENT
	gettimeofday(&t1, NULL);
#endif

	if (commit)
	{
		sqlite3_resut = sqlite3_exec(dbHandle, "END TRANSACTION", NULL, NULL, NULL);
		if (sqlite3_resut != SQLITE_OK)
		{
			raiseError("appendReadings", "Executing the commit of the transaction - error :%s:", sqlite3_errmsg(dbHandle));
			rowNumber = -1;
		}
		m_streamOpenTransaction = true;
	}

	if(stmt != NULL)
	{
		if (sqlite3_finalize(stmt) != SQLITE_OK)
		{
			raiseError("appendReadings","freeing SQLite in memory structure - error :%s:", sqlite3_errmsg(dbHandle));
		}
	}

#if INSTRUMENT
	gettimeofday(&t2, NULL);
#endif

#if INSTRUMENT
	struct timeval tm;
	double timeT1, timeT2, timeT3;

	timersub(&t1, &start, &tm);
	timeT1 = tm.tv_sec + ((double)tm.tv_usec / 1000000);

	timersub(&t2, &t1, &tm);
	timeT2 = tm.tv_sec + ((double)tm.tv_usec / 1000000);

	Logger::getLogger()->debug("readingStream row count :%d:", rowNumber);

	Logger::getLogger()->debug("readingStream Timing - stream handling %.3f seconds - commit/finalize %.3f seconds",
							   timeT1,
							   timeT2
	);
#endif

	return rowNumber;
}

/**
 * Append a set of readings to the readings table
 */
int Connection::appendReadings(const char *readings)
{
// Default template parameter uses UTF8 and MemoryPoolAllocator.
Document doc;
int      row = 0, readingId;
bool     add_row = false;

int lastReadingsId;

// Variables related to the SQLite insert using prepared command
const char   *user_ts;
const char   *asset_code;
string        reading,
              msg;
sqlite3_stmt *stmt;
int rc;
int           sqlite3_resut;
int           readingsId;
string        now;

std::pair<int, sqlite3_stmt *> pairValue;
string lastAsset;

// Retry mechanism
int retries = 0;
int sleep_time_ms = 0;

int localNReadingsTotal;

	ReadingsCatalogue *readCatalogue = ReadingsCatalogue::getInstance();

	//# FIXME_I:
	localNReadingsTotal = readCatalogue->getNReadingsTotal();
	vector<sqlite3_stmt *> readingsStmt(localNReadingsTotal +1, nullptr);
//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx4 readingsStmt size :%d: ",  localNReadingsTotal);
	Logger::getLogger()->setMinLevel("warning");

	ostringstream threadId;
	threadId << std::this_thread::get_id();

	//# FIXME_I: to be remove
	//loadAssetReadingCatalogue();
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx appendReadings start thread :%s:", threadId.str().c_str());
	Logger::getLogger()->setMinLevel("warning");


#if INSTRUMENT
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("appendReadings start thread :%s:", threadId.str().c_str());
	Logger::getLogger()->setMinLevel("warning");

	struct timeval	start, t1, t2, t3, t4, t5;
#endif

#if INSTRUMENT
	gettimeofday(&start, NULL);
#endif

	ParseResult ok = doc.Parse(readings);
	if (!ok)
	{
 		raiseError("appendReadings", GetParseError_En(doc.GetParseError()));
		return -1;
	}

	if (!doc.HasMember("readings"))
	{
 		raiseError("appendReadings", "Payload is missing a readings array");
		return -1;
	}
	Value &readingsValue = doc["readings"];
	if (!readingsValue.IsArray())
	{
		raiseError("appendReadings", "Payload is missing the readings array");
		return -1;
	}

	int tableIdx;
	string sql_cmd;

	{
	m_writeAccessOngoing.fetch_add(1);
	//unique_lock<mutex> lck(db_mutex);
	sqlite3_exec(dbHandle, "BEGIN TRANSACTION", NULL, NULL, NULL);

#if INSTRUMENT
		gettimeofday(&t1, NULL);
#endif

	lastAsset = "";
	for (Value::ConstValueIterator itr = readingsValue.Begin(); itr != readingsValue.End(); ++itr)
	{
		if (!itr->IsObject())
		{
			raiseError("appendReadings","Each reading in the readings array must be an object");
			sqlite3_exec(dbHandle, "ROLLBACK TRANSACTION;", NULL, NULL, NULL);
			return -1;
		}

		add_row = true;

		// Handles - user_ts
		char formatted_date[LEN_BUFFER_DATE] = {0};
		user_ts = (*itr)["user_ts"].GetString();
		if (strcmp(user_ts, "now()") == 0)
		{
			getNow(now);
			user_ts = now.c_str();
		}
		else
		{
			if (! formatDate(formatted_date, sizeof(formatted_date), user_ts) )
			{
				raiseError("appendReadings", "Invalid date |%s|", user_ts);
				add_row = false;
			}
			else
			{
				user_ts = formatted_date;
			}
		}

		if (add_row)
		{
			// Handles - asset_code
			asset_code = (*itr)["asset_code"].GetString();

			//# A different asset is managed respect the previous one
			if (lastAsset.compare(asset_code)!= 0)
			{
				readingsId = readCatalogue->getReadingReference(this, asset_code);

				if (readingsId >= localNReadingsTotal)
				{
					//# FIXME_I:
					localNReadingsTotal = readingsId + 1;
					readingsStmt.resize(localNReadingsTotal, nullptr);

					//# FIXME_I
					Logger::getLogger()->setMinLevel("debug");
					Logger::getLogger()->debug("xxx4 readingsStmt resize size :%d: idx :%d: ", localNReadingsTotal, readingsId);
					Logger::getLogger()->setMinLevel("warning");
				}
				//# FIXME_I
				Logger::getLogger()->setMinLevel("debug");
				Logger::getLogger()->debug("xxx4 readingsStmt size :%d: idx :%d: ", localNReadingsTotal, readingsId);
				Logger::getLogger()->setMinLevel("warning");

				//# FIXME_I:
				if (readingsStmt[readingsId] == nullptr)
				{
					string dbName = readCatalogue->getDbNameFromTableId(readingsId);
					string dbReadingsName = readCatalogue->getReadingsName(readingsId);

					sql_cmd = "INSERT INTO  " + dbName  + dbReadingsName + " ( id, user_ts, reading ) VALUES  (?,?,?)";
					rc = sqlite3_prepare_v2(dbHandle, sql_cmd.c_str(), -1, &readingsStmt[readingsId], NULL);
					if (rc != SQLITE_OK)
					{
						raiseError("appendReadings", sqlite3_errmsg(dbHandle));
					}

				}
				stmt = readingsStmt[readingsId];

				lastAsset = asset_code;
			}

			//# FIXME_I:


			// Handles - reading
			StringBuffer buffer;
			Writer<StringBuffer> writer(buffer);
			(*itr)["reading"].Accept(writer);
			reading = escape(buffer.GetString());

			if(stmt != NULL) {

				sqlite3_bind_int (stmt, 1, readCatalogue->getGlobalId());
				sqlite3_bind_text(stmt, 2, user_ts         ,-1, SQLITE_STATIC);
				sqlite3_bind_text(stmt, 3, reading.c_str(), -1, SQLITE_STATIC);

				retries =0;
				sleep_time_ms = 0;

				// Retry mechanism in case SQLlite DB is locked
				do {
					// Insert the row using a lock to ensure one insert at time
					{

						sqlite3_resut = sqlite3_step(stmt);

					}
					if (sqlite3_resut == SQLITE_LOCKED  )
					{
						sleep_time_ms = PREP_CMD_RETRY_BASE + (random() %  PREP_CMD_RETRY_BACKOFF);
						retries++;

						Logger::getLogger()->info("SQLITE_LOCKED - record :%d: - retry number :%d: sleep time ms :%d:" ,row ,retries ,sleep_time_ms);

						std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time_ms));
					}
					if (sqlite3_resut == SQLITE_BUSY)
					{
						ostringstream threadId;
						threadId << std::this_thread::get_id();

						sleep_time_ms = PREP_CMD_RETRY_BASE + (random() %  PREP_CMD_RETRY_BACKOFF);
						retries++;

						Logger::getLogger()->info("SQLITE_BUSY - thread :%s: - record :%d: - retry number :%d: sleep time ms :%d:", threadId.str().c_str() ,row, retries, sleep_time_ms);

						std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time_ms));
					}
				} while (retries < PREP_CMD_MAX_RETRIES && (sqlite3_resut == SQLITE_LOCKED || sqlite3_resut == SQLITE_BUSY));

				if (sqlite3_resut == SQLITE_DONE)
				{
					row++;

					sqlite3_clear_bindings(stmt);
					sqlite3_reset(stmt);
				}
				else
				{
					raiseError("appendReadings","Inserting a row into SQLIte using a prepared command - asset_code :%s: error :%s: reading :%s: ",
						asset_code,
						sqlite3_errmsg(dbHandle),
						reading.c_str());

					sqlite3_exec(dbHandle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
					return -1;
				}
			}
		}
	}

	sqlite3_resut = sqlite3_exec(dbHandle, "END TRANSACTION", NULL, NULL, NULL);
	if (sqlite3_resut != SQLITE_OK)
	{
		raiseError("appendReadings", "Executing the commit of the transaction :%s:", sqlite3_errmsg(dbHandle));
		row = -1;
	}
	m_writeAccessOngoing.fetch_sub(1);
	//db_cv.notify_all();
	}

#if INSTRUMENT
		gettimeofday(&t2, NULL);
#endif

	// Finalize sqlite structures
	for (auto &item : readingsStmt)
	{
		if(item != nullptr)
		{

			if (sqlite3_finalize(item) != SQLITE_OK)
			{
				raiseError("appendReadings","freeing SQLite in memory structure - error :%s:", sqlite3_errmsg(dbHandle));
			}
		}

	}

#if INSTRUMENT
		gettimeofday(&t3, NULL);
#endif

#if INSTRUMENT
		struct timeval tm;
		double timeT1, timeT2, timeT3;

		timersub(&t1, &start, &tm);
		timeT1 = tm.tv_sec + ((double)tm.tv_usec / 1000000);

		timersub(&t2, &t1, &tm);
		timeT2 = tm.tv_sec + ((double)tm.tv_usec / 1000000);

		timersub(&t3, &t2, &tm);
		timeT3 = tm.tv_sec + ((double)tm.tv_usec / 1000000);

		Logger::getLogger()->setMinLevel("debug");
		Logger::getLogger()->debug("appendReadings end   thread :%s: buffer :%10lu: count :%5d: JSON :%6.3f: inserts :%6.3f: finalize :%6.3f:",
								   threadId.str().c_str(),
								   strlen(readings),
								   row,
								   timeT1,
								   timeT2,
								   timeT3
		);
		Logger::getLogger()->setMinLevel("warning");

#endif

	return row;
}

/**
 * Fetch a block of readings from the reading table
 * It might not work with SQLite 3
 *
 * Fetch, used by the north side, returns timestamp in UTC.
 *
 * NOTE : it expects to handle a date having a fixed format
 * with milliseconds, microseconds and timezone expressed,
 * like for example :
 *
 *    2019-01-11 15:45:01.123456+01:00
 */
bool Connection::fetchReadings(unsigned long id,
			       unsigned int blksize,
			       std::string& resultSet)
{
char sqlbuffer[512];
char *zErrMsg = NULL;
int rc;
int retrieve;

	// SQL command to extract the data from the readings.readings
	const char *sql_cmd = R"(
	SELECT
		id,
		asset_code,
		reading,
		strftime('%%Y-%%m-%%d %%H:%%M:%%S', user_ts, 'utc')  ||
		substr(user_ts, instr(user_ts, '.'), 7) AS user_ts,
		strftime('%%Y-%%m-%%d %%H:%%M:%%f', ts, 'utc') AS ts
	FROM  )" DB_READINGS R"(.readings_1
	WHERE id >= %lu
	ORDER BY id ASC
	LIMIT %u;
	)";

	/*
	 * This query assumes datetime values are in 'localtime'
	 */
	snprintf(sqlbuffer,
		 sizeof(sqlbuffer),
		 sql_cmd,
		 id,
		 blksize);
	logSQL("ReadingsFetch", sqlbuffer);
	sqlite3_stmt *stmt;
	// Prepare the SQL statement and get the result set
	if (sqlite3_prepare_v2(dbHandle,
			       sqlbuffer,
			       -1,
			       &stmt,
			       NULL) != SQLITE_OK)
	{
		raiseError("retrieve", sqlite3_errmsg(dbHandle));

		// Failure
		return false;
	}
	else
	{
		// Call result set mapping
		rc = mapResultSet(stmt, resultSet);

		// Delete result set
		sqlite3_finalize(stmt);

		// Check result set errors
		if (rc != SQLITE_DONE)
		{
			raiseError("retrieve", sqlite3_errmsg(dbHandle));

			// Failure
			return false;
               	}
		else
		{
			// Success
			return true;
		}
	}
}


/**
 * Perform a query against the readings table
 *
 * retrieveReadings, used by the API, returns timestamp in localtime.
 *
 */
bool Connection::retrieveReadings(const string& condition, string& resultSet)
{
// Default template parameter uses UTF8 and MemoryPoolAllocator.
Document	document;
SQLBuffer	sql;
// Extra constraints to add to where clause
SQLBuffer	jsonConstraints;
bool		isAggregate = false;


	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx retrieveReadings ");
	Logger::getLogger()->setMinLevel("warning");

	try {
		if (dbHandle == NULL)
		{
			raiseError("retrieve", "No SQLite 3 db connection available");
			return false;
		}

		if (condition.empty())
		{
			const char *sql_cmd = R"(
					SELECT
						id,
						asset_code,
						reading,
						strftime(')" F_DATEH24_SEC R"(', user_ts, 'localtime')  ||
						substr(user_ts, instr(user_ts, '.'), 7) AS user_ts,
						strftime(')" F_DATEH24_MS R"(', ts, 'localtime') AS ts
					FROM )" DB_READINGS R"(.readings_1)";

			sql.append(sql_cmd);
		}
		else
		{
			if (document.Parse(condition.c_str()).HasParseError())
			{
				raiseError("retrieve", "Failed to parse JSON payload");
				return false;
			}

			// timebucket aggregate all datapoints
			if (aggregateAll(document))
			{       
				return aggregateQuery(document, resultSet);
			}

			if (document.HasMember("aggregate"))
			{
				isAggregate = true;
				sql.append("SELECT ");
				if (document.HasMember("modifier"))
				{
					sql.append(document["modifier"].GetString());
					sql.append(' ');
				}
				if (!jsonAggregates(document, document["aggregate"], sql, jsonConstraints, true))
				{
					return false;
				}
				sql.append(" FROM  " DB_READINGS ".");
			}
			else if (document.HasMember("return"))
			{
				int col = 0;
				Value& columns = document["return"];
				if (! columns.IsArray())
				{
					raiseError("retrieve", "The property return must be an array");
					return false;
				}
				sql.append("SELECT ");
				if (document.HasMember("modifier"))
				{
					sql.append(document["modifier"].GetString());
					sql.append(' ');
				}
				for (Value::ConstValueIterator itr = columns.Begin(); itr != columns.End(); ++itr)
				{
					if (col)
						sql.append(", ");
					if (!itr->IsObject())	// Simple column name
					{
						if (strcmp(itr->GetString() ,"user_ts") == 0)
						{
							// Display without TZ expression and microseconds also
							sql.append(" strftime('" F_DATEH24_SEC "', user_ts, 'localtime') ");
							sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
							sql.append(" as  user_ts ");
						}
						else if (strcmp(itr->GetString() ,"ts") == 0)
						{
							// Display without TZ expression and microseconds also
							sql.append(" strftime('" F_DATEH24_MS "', ts, 'localtime') ");
							sql.append(" as ts ");
						}
						else
						{
							sql.append(itr->GetString());
						}
					}
					else
					{
						if (itr->HasMember("column"))
						{
							if (! (*itr)["column"].IsString())
							{
								raiseError("retrieve",
									   "column must be a string");
								return false;
							}
							if (itr->HasMember("format"))
							{
								if (! (*itr)["format"].IsString())
								{
									raiseError("retrieve",
										   "format must be a string");
									return false;
								}

								// SQLite 3 date format.
								string new_format;
								applyColumnDateFormatLocaltime((*itr)["format"].GetString(),
										      (*itr)["column"].GetString(),
										      new_format, true);
								// Add the formatted column or use it as is
								sql.append(new_format);
							}
							else if (itr->HasMember("timezone"))
							{
								if (! (*itr)["timezone"].IsString())
								{
									raiseError("retrieve",
										   "timezone must be a string");
									return false;
								}
								// SQLite3 doesnt support time zone formatting
								const char *tz = (*itr)["timezone"].GetString();

								if (strncasecmp(tz, "utc", 3) == 0)
								{
									if (strcmp((*itr)["column"].GetString() ,"user_ts") == 0)
									{
										// Extract milliseconds and microseconds for the user_ts fields

										sql.append("strftime('" F_DATEH24_SEC "', user_ts, 'utc') ");
										sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
										if (! itr->HasMember("alias"))
										{
											sql.append(" AS ");
											sql.append((*itr)["column"].GetString());
										}
									}
									else
									{
										sql.append("strftime('" F_DATEH24_MS "', ");
										sql.append((*itr)["column"].GetString());
										sql.append(", 'utc')");
										if (! itr->HasMember("alias"))
										{
											sql.append(" AS ");
											sql.append((*itr)["column"].GetString());
										}
									}
								}
								else if (strncasecmp(tz, "localtime", 9) == 0)
								{
									if (strcmp((*itr)["column"].GetString() ,"user_ts") == 0)
									{
										// Extract milliseconds and microseconds for the user_ts fields

										sql.append("strftime('" F_DATEH24_SEC "', user_ts, 'localtime') ");
										sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
										if (! itr->HasMember("alias"))
										{
											sql.append(" AS ");
											sql.append((*itr)["column"].GetString());
										}
									}
									else
									{
										sql.append("strftime('" F_DATEH24_MS "', ");
										sql.append((*itr)["column"].GetString());
										sql.append(", 'localtime')");
										if (! itr->HasMember("alias"))
										{
											sql.append(" AS ");
											sql.append((*itr)["column"].GetString());
										}
									}
								}
								else
								{
									raiseError("retrieve",
										   "SQLite3 plugin does not support timezones in queries");
									return false;
								}
							}
							else
							{

								if (strcmp((*itr)["column"].GetString() ,"user_ts") == 0)
								{
									// Extract milliseconds and microseconds for the user_ts fields

									sql.append("strftime('" F_DATEH24_SEC "', user_ts, 'localtime') ");
									sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
									if (! itr->HasMember("alias"))
									{
										sql.append(" AS ");
										sql.append((*itr)["column"].GetString());
									}
								}
								else
								{
									sql.append("strftime('" F_DATEH24_MS "', ");
									sql.append((*itr)["column"].GetString());
									sql.append(", 'localtime')");
									if (! itr->HasMember("alias"))
									{
										sql.append(" AS ");
										sql.append((*itr)["column"].GetString());
									}
								}
							}
							sql.append(' ');
						}
						else if (itr->HasMember("json"))
						{
							const Value& json = (*itr)["json"];
							if (! returnJson(json, sql, jsonConstraints))
								return false;
						}
						else
						{
							raiseError("retrieve",
								   "return object must have either a column or json property");
							return false;
						}

						if (itr->HasMember("alias"))
						{
							sql.append(" AS \"");
							sql.append((*itr)["alias"].GetString());
							sql.append('"');
						}
					}
					col++;
				}
				sql.append(" FROM  " DB_READINGS ".");
			}
			else
			{
				sql.append("SELECT ");
				if (document.HasMember("modifier"))
				{
					sql.append(document["modifier"].GetString());
					sql.append(' ');
				}

				const char *sql_cmd = R"(
						id,
						asset_code,
						reading,
						strftime(')" F_DATEH24_SEC R"(', user_ts, 'localtime')  ||
						substr(user_ts, instr(user_ts, '.'), 7) AS user_ts,
						strftime(')" F_DATEH24_MS R"(', ts, 'localtime') AS ts
                    FROM  )" DB_READINGS R"(.)";

				sql.append(sql_cmd);
			}
			//# FIXME_I:
			sql.append("readings_1");

			if (document.HasMember("where"))
			{
				sql.append(" WHERE ");
			 
				if (document.HasMember("where"))
				{
					if (!jsonWhereClause(document["where"], sql))
					{
						return false;
					}
				}
				else
				{
					raiseError("retrieve",
						   "JSON does not contain where clause");
					return false;
				}
				if (! jsonConstraints.isEmpty())
				{
					sql.append(" AND ");
                                        const char *jsonBuf =  jsonConstraints.coalesce();
                                        sql.append(jsonBuf);
                                        delete[] jsonBuf;
				}
			}
			else if (isAggregate)
			{
				/*
				 * Performance improvement: force sqlite to use an index
				 * if we are doing an aggregate and have no where clause.
				 */
				sql.append(" WHERE asset_code = asset_code");
			}
			if (!jsonModifiers(document, sql, true))
			{
				return false;
			}
		}
		sql.append(';');

		const char *query = sql.coalesce();
		char *zErrMsg = NULL;
		int rc;
		sqlite3_stmt *stmt;

		logSQL("ReadingsRetrieve", query);

		// Prepare the SQL statement and get the result set
		rc = sqlite3_prepare_v2(dbHandle, query, -1, &stmt, NULL);

		// Release memory for 'query' var
		delete[] query;

		if (rc != SQLITE_OK)
		{
			raiseError("retrieve", sqlite3_errmsg(dbHandle));
			return false;
		}

		// Call result set mapping
		rc = mapResultSet(stmt, resultSet);

		// Delete result set
		sqlite3_finalize(stmt);

		// Check result set mapping errors
		if (rc != SQLITE_DONE)
		{
			raiseError("retrieve", sqlite3_errmsg(dbHandle));
			// Failure
			return false;
		}
		// Success
		return true;
	} catch (exception e) {
		raiseError("retrieve", "Internal error: %s", e.what());
	}

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx retrieveReadings end ");
	Logger::getLogger()->setMinLevel("warning");

}

/**
 * Purge readings from the reading table
 */
unsigned int  Connection::purgeReadings(unsigned long age,
					unsigned int flags,
					unsigned long sent,
					std::string& result)
{
long unsentPurged = 0;
long unsentRetained = 0;
long numReadings = 0;
unsigned long rowidLimit = 0, minrowidLimit = 0, maxrowidLimit = 0, rowidMin;
struct timeval startTv, endTv;
int blocks = 0;

	Logger *logger = Logger::getLogger();

	result = "{ \"removed\" : 0, ";
	result += " \"unsentPurged\" : 0, ";
	result += " \"unsentRetained\" : 0, ";
	result += " \"readings\" : 0 }";

	logger->info("Purge starting...");
	gettimeofday(&startTv, NULL);
	/*
	 * We fetch the current rowid and limit the purge process to work on just
	 * those rows present in the database when the purge process started.
	 * This provents us looping in the purge process if new readings become
	 * eligible for purging at a rate that is faster than we can purge them.
	 */
	{
		char *zErrMsg = NULL;
		int rc;
		rc = SQLexec(dbHandle,
		     "select max(rowid) from " DB_READINGS ".readings_1;",
	  	     rowidCallback,
		     &rowidLimit,
		     &zErrMsg);

		if (rc != SQLITE_OK)
		{
 			raiseError("purge - phase 0, fetching rowid limit ", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}
		maxrowidLimit = rowidLimit;
	}

	{
		char *zErrMsg = NULL;
		int rc;
		rc = SQLexec(dbHandle,
		     "select min(rowid) from " DB_READINGS ".readings_1;",
	  	     rowidCallback,
		     &minrowidLimit,
		     &zErrMsg);

		if (rc != SQLITE_OK)
		{
 			raiseError("purge - phaase 0, fetching minrowid limit ", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}
	}

	if (age == 0)
	{
		/*
		 * An age of 0 means remove the oldest hours data.
		 * So set age based on the data we have and continue.
		 */
		SQLBuffer oldest;
		oldest.append("SELECT (strftime('%s','now', 'utc') - strftime('%s', MIN(user_ts)))/360 FROM " DB_READINGS ".readings_1 where rowid <= ");
		oldest.append(rowidLimit);
		oldest.append(';');
		const char *query = oldest.coalesce();
		char *zErrMsg = NULL;
		int rc;
		int purge_readings = 0;

		// Exec query and get result in 'purge_readings' via 'selectCallback'
		rc = SQLexec(dbHandle,
			     query,
			     selectCallback,
			     &purge_readings,
			     &zErrMsg);
		// Release memory for 'query' var
		delete[] query;

		if (rc == SQLITE_OK)
		{
			age = purge_readings;
		}
		else
		{
 			raiseError("purge - phase 1", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}
	}

	{
		/*
		 * Refine rowid limit to just those rows older than age hours.
		 */
		char *zErrMsg = NULL;
		int rc;
		unsigned long l = minrowidLimit;
		unsigned long r = ((flags & 0x01) && sent) ? min(sent, rowidLimit) : rowidLimit;
		r = max(r, l);
		//logger->info("%s:%d: l=%u, r=%u, sent=%u, rowidLimit=%u, minrowidLimit=%u, flags=%u", __FUNCTION__, __LINE__, l, r, sent, rowidLimit, minrowidLimit, flags);
		if (l == r)
		{
 			logger->info("No data to purge: min_id == max_id == %u", minrowidLimit);
			return 0;
		}

		unsigned long m=l;

		while (l <= r)
		{
			unsigned long midRowId = 0;
			unsigned long prev_m = m;
		    	m = l + (r - l) / 2;
			if (prev_m == m) break;

			// e.g. select id from readings where rowid = 219867307 AND user_ts < datetime('now' , '-24 hours', 'utc');
			SQLBuffer sqlBuffer;
			sqlBuffer.append("select id from " DB_READINGS ".readings_1 where rowid = ");
			sqlBuffer.append(m);
			sqlBuffer.append(" AND user_ts < datetime('now' , '-");
			sqlBuffer.append(age);
			sqlBuffer.append(" hours');");
			const char *query = sqlBuffer.coalesce();


			rc = SQLexec(dbHandle,
			query,
			rowidCallback,
			&midRowId,
			&zErrMsg);

			if (rc != SQLITE_OK)
			{
	 			raiseError("purge - phase 1, fetching midRowId ", zErrMsg);
				sqlite3_free(zErrMsg);
				return 0;
			}

			if (midRowId == 0) // mid row doesn't satisfy given condition for user_ts, so discard right/later half and look in left/earlier half
			{
				// search in earlier/left half
				r = m - 1;

				// The m position should be skipped as midRowId is 0
				m = r;
			}
			else //if (l != m)
			{
				// search in later/right half
		        	l = m + 1;
			}
		}

		rowidLimit = m;

		if (minrowidLimit == rowidLimit)
		{
 			logger->info("No data to purge");
			return 0;
		}

		rowidMin = minrowidLimit;
	}
	//logger->info("Purge collecting unsent row count");
	if ((flags & 0x01) == 0)
	{
		char *zErrMsg = NULL;
		int rc;
		int lastPurgedId;
		SQLBuffer idBuffer;
		idBuffer.append("select id from " DB_READINGS ".readings_1 where rowid = ");
		idBuffer.append(rowidLimit);
		idBuffer.append(';');
		const char *idQuery = idBuffer.coalesce();
		rc = SQLexec(dbHandle,
		     idQuery,
	  	     rowidCallback,
		     &lastPurgedId,
		     &zErrMsg);

		// Release memory for 'idQuery' var
		delete[] idQuery;

		if (rc != SQLITE_OK)
		{
 			raiseError("purge - phase 0, fetching rowid limit ", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}

		if (sent != 0 && lastPurgedId > sent)	// Unsent readings will be purged
		{
			// Get number of unsent rows we are about to remove
			int unsent = rowidLimit - sent;
			unsentPurged = unsent;
		}
	}
	if (m_writeAccessOngoing)
	{
		while (m_writeAccessOngoing)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	unsigned int deletedRows = 0;
	char *zErrMsg = NULL;
	unsigned int rowsAffected, totTime=0, prevBlocks=0, prevTotTime=0;
	logger->info("Purge about to delete readings # %ld to %ld", rowidMin, rowidLimit);
	while (rowidMin < rowidLimit)
	{
		blocks++;
		rowidMin += purgeBlockSize;
		if (rowidMin > rowidLimit)
		{
			rowidMin = rowidLimit;
		}
		SQLBuffer sql;
		sql.append("DELETE FROM " DB_READINGS ".readings_1 WHERE rowid <= ");
		sql.append(rowidMin);
		sql.append(';');
		const char *query = sql.coalesce();
		logSQL("ReadingsPurge", query);

		int rc;
		{
		//unique_lock<mutex> lck(db_mutex);
//		if (m_writeAccessOngoing) db_cv.wait(lck);

		START_TIME;
		// Exec DELETE query: no callback, no resultset
		rc = SQLexec(dbHandle,
			     query,
			     NULL,
			     NULL,
			     &zErrMsg);
		END_TIME;

		// Release memory for 'query' var
		delete[] query;

		totTime += usecs;

		if(usecs>150000)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100+usecs/10000));
		}
		}

		if (rc != SQLITE_OK)
		{
			raiseError("purge - phase 3", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}

		// Get db changes
		rowsAffected = sqlite3_changes(dbHandle);
		deletedRows += rowsAffected;
		logger->debug("Purge delete block #%d with %d readings", blocks, rowsAffected);

		if(blocks % RECALC_PURGE_BLOCK_SIZE_NUM_BLOCKS == 0)
		{
			int prevAvg = prevTotTime/(prevBlocks?prevBlocks:1);
			int currAvg = (totTime-prevTotTime)/(blocks-prevBlocks);
			int avg = ((prevAvg?prevAvg:currAvg)*5 + currAvg*5) / 10; // 50% weightage for long term avg and 50% weightage for current avg
			prevBlocks = blocks;
			prevTotTime = totTime;
			int deviation = abs(avg - TARGET_PURGE_BLOCK_DEL_TIME);
			logger->debug("blocks=%d, totTime=%d usecs, prevAvg=%d usecs, currAvg=%d usecs, avg=%d usecs, TARGET_PURGE_BLOCK_DEL_TIME=%d usecs, deviation=%d usecs", 
							blocks, totTime, prevAvg, currAvg, avg, TARGET_PURGE_BLOCK_DEL_TIME, deviation);
			if (deviation > TARGET_PURGE_BLOCK_DEL_TIME/10)
			{
				float ratio = (float)TARGET_PURGE_BLOCK_DEL_TIME / (float)avg;
				if (ratio > 2.0) ratio = 2.0;
				if (ratio < 0.5) ratio = 0.5;
				purgeBlockSize = (float)purgeBlockSize * ratio;
				purgeBlockSize = purgeBlockSize / PURGE_BLOCK_SZ_GRANULARITY * PURGE_BLOCK_SZ_GRANULARITY;
				if (purgeBlockSize < MIN_PURGE_DELETE_BLOCK_SIZE)
					purgeBlockSize = MIN_PURGE_DELETE_BLOCK_SIZE;
				if (purgeBlockSize > MAX_PURGE_DELETE_BLOCK_SIZE)
					purgeBlockSize = MAX_PURGE_DELETE_BLOCK_SIZE;
				logger->debug("Changed purgeBlockSize to %d", purgeBlockSize);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		//Logger::getLogger()->debug("Purge delete block #%d with %d readings", blocks, rowsAffected);
	} while (rowidMin  < rowidLimit);

	unsentRetained = maxrowidLimit - rowidLimit;

	numReadings = maxrowidLimit +1 - minrowidLimit - deletedRows;

	if (sent == 0)	// Special case when not north process is used
	{
		unsentPurged = deletedRows;
	}

	ostringstream convert;

	convert << "{ \"removed\" : " << deletedRows << ", ";
	convert << " \"unsentPurged\" : " << unsentPurged << ", ";
	convert << " \"unsentRetained\" : " << unsentRetained << ", ";
    	convert << " \"readings\" : " << numReadings << " }";

	result = convert.str();

	//logger->debug("Purge result=%s", result.c_str());

	gettimeofday(&endTv, NULL);
	unsigned long duration = (1000000 * (endTv.tv_sec - startTv.tv_sec)) + endTv.tv_usec - startTv.tv_usec;
	logger->info("Purge process complete in %d blocks in %lduS", blocks, duration);

	return deletedRows;
}


/**
 * Purge readings from the reading table
 */
unsigned int  Connection::purgeReadingsByRows(unsigned long rows,
					unsigned int flags,
					unsigned long sent,
					std::string& result)
{
unsigned long  deletedRows = 0, unsentPurged = 0, unsentRetained = 0, numReadings = 0;
unsigned long limit = 0;

	Logger *logger = Logger::getLogger();

	logger->info("Purge by Rows called");
	if ((flags & 0x01) == 0x01)
	{
		limit = sent;
		logger->info("Sent is %d", sent);
	}
	logger->info("Purge by Rows called with flags %x, rows %d, limit %d", flags, rows, limit);
	// Don't save unsent rows
	int rowcount;
	do {
		char *zErrMsg = NULL;
		int rc;
		rc = SQLexec(dbHandle,
		     "select count(rowid) from readings;",
		     rowidCallback,
		     &rowcount,
		     &zErrMsg);

		if (rc != SQLITE_OK)
		{
			raiseError("purge - phaase 0, fetching row count", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}
		if (rowcount <= rows)
		{
			logger->info("Row count %d is less than required rows %d", rowcount, rows);
			break;
		}
		int minId;
		rc = SQLexec(dbHandle,
		     "select min(id) from readings;",
		     rowidCallback,
		     &minId,
		     &zErrMsg);

		if (rc != SQLITE_OK)
		{
			raiseError("purge - phaase 0, fetching minimum id", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}
		int maxId;
		rc = SQLexec(dbHandle,
		     "select max(id) from readings;",
		     rowidCallback,
		     &maxId,
		     &zErrMsg);

		if (rc != SQLITE_OK)
		{
			raiseError("purge - phaase 0, fetching maximum id", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}
		int deletePoint = minId + 10000;
		if (maxId - deletePoint < rows || deletePoint > maxId)
			deletePoint = maxId - rows;
		if (limit && limit > deletePoint)
		{
			deletePoint = limit;
		}
		SQLBuffer sql;

		logger->info("RowCount %d, Max Id %d, min Id %d, delete point %d", rowcount, maxId, minId, deletePoint);

		sql.append("delete from readings where id <= ");
		sql.append(deletePoint);
		const char *query = sql.coalesce();
		{
			//unique_lock<mutex> lck(db_mutex);
//			if (m_writeAccessOngoing) db_cv.wait(lck);

			// Exec DELETE query: no callback, no resultset
			rc = SQLexec(dbHandle, query, NULL, NULL, &zErrMsg);
			int rowsAffected = sqlite3_changes(dbHandle);
			deletedRows += rowsAffected;
			numReadings = rowcount - rowsAffected;
			// Release memory for 'query' var
			delete[] query;
			logger->debug("Deleted %d rows", rowsAffected);
			if (rowsAffected == 0)
			{
				break;
			}
			if (limit != 0 && sent != 0)
			{
				unsentPurged = deletePoint - sent;
			}
			else if (!limit)
			{
				unsentPurged += rowsAffected;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (rowcount > rows);

	if (limit)
	{
		unsentRetained = numReadings - rows;
	}


	ostringstream convert;

	convert << "{ \"removed\" : " << deletedRows << ", ";
	convert << " \"unsentPurged\" : " << unsentPurged << ", ";
	convert << " \"unsentRetained\" : " << unsentRetained << ", ";
    	convert << " \"readings\" : " << numReadings << " }";

	result = convert.str();
	logger->info("Purge by Rows complete: %s", result.c_str());
	return deletedRows;
}

/**
 * # FIXME_I:
 */
void ReadingsCatalogue::raiseError(const char *operation, const char *reason, ...)
{
	char	tmpbuf[512];

	va_list ap;
	va_start(ap, reason);
	vsnprintf(tmpbuf, sizeof(tmpbuf), reason, ap);
	va_end(ap);
	Logger::getLogger()->error("ReadingsCatalogues error: %s", tmpbuf);
}

//# FIXME_I:
// Retrieves the global_id from thd DB, if it is -1 it should be calculated
bool ReadingsCatalogue::evaluateGlobalId ()
{
	string sql_cmd;
	int rc;
	int id;
	int nCols;
	sqlite3_stmt *stmt;
	sqlite3 *dbHandle;

	ConnectionManager *manager = ConnectionManager::getInstance();
	Connection *connection = manager->allocate();
	dbHandle = connection->getDbHandle();

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx3 evaluateGlobalId");
	Logger::getLogger()->setMinLevel("warning");

	// Retrieves the global_id from thd DB
	{
		sql_cmd = " SELECT global_id FROM " DB_READINGS ".configuration_readings ";

		if (sqlite3_prepare_v2(dbHandle,sql_cmd.c_str(),-1, &stmt,NULL) != SQLITE_OK)
		{
			raiseError("evaluateGlobalId", sqlite3_errmsg(dbHandle));
			return false;
		}

		if (SQLStep(stmt) != SQLITE_ROW)
		{
			m_globalId = 1;

			sql_cmd = " INSERT INTO " DB_READINGS ".configuration_readings VALUES (" + to_string(m_globalId) + ")";

			if (SQLexec(dbHandle,sql_cmd.c_str()) != SQLITE_OK)
			{
				raiseError("evaluateGlobalId", sqlite3_errmsg(dbHandle));
				return false;
			}
		}
		else
		{
			nCols = sqlite3_column_count(stmt);
			m_globalId = sqlite3_column_int(stmt, 0);
		}
	}

	if ( m_globalId == -1)
	{
		m_globalId = calculateGlobalId (dbHandle);
	}

	id = m_globalId;
	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx3 global id from the DB :%d:", id);
	Logger::getLogger()->setMinLevel("warning");


	// Set the global_id in the DB to -1 to force a calculation at the restart
	// in case the shutdown is not executed and the proper value stored
	{
		sql_cmd = " UPDATE " DB_READINGS ".configuration_readings SET global_id=-1;";

		if (SQLexec(dbHandle,sql_cmd.c_str()) != SQLITE_OK)
		{
			raiseError("evaluateGlobalId", sqlite3_errmsg(dbHandle));
			return false;
		}
	}

	sqlite3_finalize(stmt);
	manager->release(connection);

	return true;
}

//# FIXME_I:
// Stores the global_id into the DB
bool ReadingsCatalogue::storeGlobalId ()
{
	string sql_cmd;
	int rc;
	int id;
	int nCols;
	sqlite3_stmt *stmt;
	sqlite3 *dbHandle;

	ConnectionManager *manager = ConnectionManager::getInstance();
	Connection *connection = manager->allocate();
	dbHandle = connection->getDbHandle();

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx3 storeGlobalId");
	Logger::getLogger()->setMinLevel("warning");

	sql_cmd = " UPDATE " DB_READINGS ".configuration_readings SET global_id=-" + to_string(m_globalId);

	if (SQLexec(dbHandle,sql_cmd.c_str()) != SQLITE_OK)
	{
		raiseError("evaluateGlobalId", sqlite3_errmsg(dbHandle));
		return false;
	}

	manager->release(connection);

	return true;
}


int ReadingsCatalogue::calculateGlobalId (sqlite3 *dbHandle)
{
	string sql_cmd;
	int rc;
	int id;
	int nCols;

	sqlite3_stmt *stmt;
	id = 1;

	// Prepare the sql command to calculate the global id from the rows in the DB
	{
		sql_cmd = R"(
			SELECT
				max(id) id
			FROM
			(
		)";

		bool firstRow = true;
		if (m_AssetReadingCatalogue.empty())
		{
			sql_cmd += " SELECT max(id) id FROM " DB_READINGS ".readings_1 ";
		}
		else
		{
			for (auto &item : m_AssetReadingCatalogue)
			{
				if (!firstRow)
					sql_cmd += " UNION ";

				sql_cmd += " SELECT max(id) id FROM " DB_READINGS ".readings_" + to_string(item.second.first) + " ";
				firstRow = false;
			}
		}
		sql_cmd += ") AS tb";
	}


	if (sqlite3_prepare_v2(dbHandle,sql_cmd.c_str(),-1, &stmt,NULL) != SQLITE_OK)
	{
		raiseError("evaluateGlobalId", sqlite3_errmsg(dbHandle));
		return false;
	}

	if (SQLStep(stmt) != SQLITE_ROW)
	{
		id = 1;
	}
	else
	{
		nCols = sqlite3_column_count(stmt);
		id = sqlite3_column_int(stmt, 0);
		// m_globalId stores then next value to be used
		id++;
	}

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx evaluateGlobalId - global id evaluated :%d:", id);
	Logger::getLogger()->setMinLevel("warning");

	sqlite3_finalize(stmt);

	return (id);
}

bool  ReadingsCatalogue::loadAssetReadingCatalogue()
{
	int nCols;
	int tableId, usedReadings, dbId, maxDbID;
	char *asset_name;
	sqlite3_stmt *stmt;
	int rc;
	sqlite3		*dbHandle;

	ostringstream threadId;
	threadId << std::this_thread::get_id();

	ConnectionManager *manager = ConnectionManager::getInstance();
	Connection        *connection = manager->allocate();
	dbHandle = connection->getDbHandle();

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx loadAssetReadingCatalogue");
	Logger::getLogger()->setMinLevel("warning");

	// loads readings catalog from the db
	const char *sql_cmd = R"(
		SELECT
			table_id,
			db_id,
			asset_code
		FROM  )" DB_READINGS R"(.asset_reading_catalogue
		ORDER BY table_id;
	)";

	//# FIXME_I:
	maxDbID = 1;
	if (sqlite3_prepare_v2(dbHandle,sql_cmd,-1, &stmt,NULL) != SQLITE_OK)
	{
		raiseError("retrieve asset_reading_catalogue", sqlite3_errmsg(dbHandle));
		return false;
	}
	else
	{
		usedReadings = 0;
		// Iterate over all the rows in the resultSet
		while ((rc = SQLStep(stmt)) == SQLITE_ROW)
		{
			nCols = sqlite3_column_count(stmt);

			tableId = sqlite3_column_int(stmt, 0);
			dbId = sqlite3_column_int(stmt, 1);
			asset_name = (char *)sqlite3_column_text(stmt, 2);

			if (dbId > maxDbID)
				maxDbID = dbId;

			//# FIXME_I
			Logger::getLogger()->setMinLevel("debug");
			Logger::getLogger()->debug("xxx read from the catalogue - thread :%s: - reading Id :%d: db Id :%d: asset name :%s: max db Id :%d:", threadId.str().c_str(), tableId, dbId,  asset_name, maxDbID);
			Logger::getLogger()->setMinLevel("warning");

			auto newItem = make_pair(tableId,dbId);
			auto newMapValue = make_pair(asset_name,newItem);
			m_AssetReadingCatalogue.insert(newMapValue);

			//# FIXME_I:
			usedReadings++;
		}

		sqlite3_finalize(stmt);
	}
	manager->release(connection);
	//# FIXME_I:
	m_dbId = maxDbID;

	return true;
}


void ReadingsCatalogue::preallocateReadingsTables()
{
	int lastReadings;
	int tableCount;
	int readingsToAllocate;
	int readingsToCreate;
	int startId;


	string dbName;

	ConnectionManager *manager = ConnectionManager::getInstance();
	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx10 preallocateReadingsTables step1");
	Logger::getLogger()->setMinLevel("warning");

	Connection        *connection = manager->allocate();

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx10 preallocateReadingsTables step1");
	Logger::getLogger()->setMinLevel("warning");


	// Identifies last readings available
	evaluateLastReadingAvailable(connection, m_dbId, &lastReadings, &tableCount);
	readingsToAllocate = getnReadingsAllocate();

	if (tableCount < readingsToAllocate)
	{
		readingsToCreate = readingsToAllocate - tableCount;
		startId = lastReadings + 1;
		createReadingsTables(1, startId, readingsToCreate);
	}

	//# FIXME_I:
	m_nReadingsUsed = m_AssetReadingCatalogue.size();
	m_nReadingsAvailable = readingsToAllocate - getUsedTablesDbId(m_dbId);

	manager->release(connection);
}

/**
 * # FIXME_I:
 */
bool  ReadingsCatalogue::createReadingsTablesNewDB()
{
	int rc;
	int nTables;
	int startId;

	int readingsToAllocate;
	int lastReadings;
	int tableCount;
	int readingsToCreate;

	string sqlCmd;
	string dbPathReadings;
	string dbAlias;
	sqlite3 *dbHandle;
	struct stat st;
	bool dbAlreadyPresent;

	char *defaultReadingsConnection;
	char defaultReadingsConnectionTmp[1000];

	m_dbId++;

	// Define the db path
	{
		defaultReadingsConnection = getenv("DEFAULT_SQLITE_DB_READINGS_FILE");

		if (defaultReadingsConnection == NULL)
		{
			dbPathReadings = getDataDir();
		}
		else
		{
			// dirname modify the content of the parameter
			strncpy ( defaultReadingsConnectionTmp, defaultReadingsConnection, sizeof(defaultReadingsConnectionTmp) );
			dbPathReadings  = dirname(defaultReadingsConnectionTmp);
		}

		if (dbPathReadings.back() != '/')
			dbPathReadings += "/";

		dbPathReadings += READINGS_DB_NAME_BASE "_" + to_string (m_dbId) + ".db";
	}

	dbAlreadyPresent = false;
	if(stat(dbPathReadings.c_str(),&st) == 0)
	{
		Logger::getLogger()->info("database file :%s: already present, creation skipped " , dbPathReadings.c_str() );
		dbAlreadyPresent = true;
	}
	else
	{
		dbAlias = READINGS_DB_NAME_BASE "_" + to_string(m_dbId);

		rc = sqlite3_open(dbPathReadings.c_str(), &dbHandle);
		if(rc != SQLITE_OK)
		{
			raiseError("createReadingsTablesNewDB", sqlite3_errmsg(dbHandle));
			return false;
		}
		sqlite3_close(dbHandle);
	}

	//# FIXME_I:

	readingsToAllocate = getnReadingsAllocate();

	if (dbAlreadyPresent)
	{
		ConnectionManager *manager = ConnectionManager::getInstance();
		Connection        *connection = manager->allocate();

		evaluateLastReadingAvailable(connection, m_dbId, &lastReadings, &tableCount);
		manager->release(connection);

		if (tableCount < readingsToAllocate)
		{
			readingsToCreate = readingsToAllocate - tableCount;
			startId = lastReadings + 1;
			createReadingsTables(1, startId, readingsToCreate);
		}
	}
	else
	{
		startId = getMaxReadingsId() + 1;
	}

	ConnectionManager *manager = ConnectionManager::getInstance();

	manager->attachNewDb(dbPathReadings, dbAlias);

	createReadingsTables(m_dbId ,startId, readingsToAllocate);
	m_nReadingsAvailable = readingsToAllocate;

	return true;
}

bool  ReadingsCatalogue::createReadingsTables(int dbId, int idStartFrom, int nTables)
{
	string createReadings, createReadingsIdx;
	string dbName;
	string dbReadingsName;
	int tableId;
	int rc;
	int readingsIdx;

	sqlite3 *dbHandle;

	ConnectionManager *manager = ConnectionManager::getInstance();
	Connection        *connection = manager->allocate();
	dbHandle = connection->getDbHandle();

	Logger *logger = Logger::getLogger();

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx createReadingsTables - MULTI DB - start");
	Logger::getLogger()->setMinLevel("warning");

	logger->info("Creating :%d: readings table in advance", nTables);

	dbName = getDbName(dbId);

	for (readingsIdx = 0 ;  readingsIdx < nTables; ++readingsIdx)
	{
		tableId = idStartFrom + readingsIdx;
		dbReadingsName = getReadingsName(tableId);

		createReadings = R"(
			CREATE TABLE )" + dbName + dbReadingsName + R"( (
				id         INTEGER                     PRIMARY KEY AUTOINCREMENT,
				reading    JSON                        NOT NULL DEFAULT '{}',
				user_ts    DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f+00:00', 'NOW')),
				ts         DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f+00:00', 'NOW'))
			);
		)";

		createReadingsIdx = R"(
			CREATE INDEX )" + dbName + dbReadingsName + R"(_ix3 ON readings_)" + to_string(tableId) + R"( (user_ts);
		)";

		rc = SQLexec(dbHandle, createReadings.c_str());
		if (rc != SQLITE_OK)
		{
			raiseError("createReadingsTables", sqlite3_errmsg(dbHandle));
			return false;
		}

		rc = SQLexec(dbHandle, createReadingsIdx.c_str());
		if (rc != SQLITE_OK)
		{
			raiseError("createReadingsTables", sqlite3_errmsg(dbHandle));
			return false;
		}
	}

	manager->release(connection);

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx createReadingsTables - end");
	Logger::getLogger()->setMinLevel("warning");
	return true;
}

void  ReadingsCatalogue::evaluateLastReadingAvailable(Connection *connection, int m_dbId, int *maxId, int *tableCount)
{
	string dbName;
	int nCols;
	int id;
	char *asset_name;
	sqlite3_stmt *stmt;
	int rc;
	string tableName;
	sqlite3		*dbHandle;

	//# FIXME_I:
	vector<int> readingsId(getNReadingsAvailable(), 0);

	dbHandle = connection->getDbHandle();
	dbName = getDbName(m_dbId);

	string sql_cmd = R"(
		SELECT name
		FROM  )" + dbName +  R"(.sqlite_master
		WHERE type='table' and name like 'readings_%';
	)";

	if (sqlite3_prepare_v2(dbHandle,sql_cmd.c_str(),-1, &stmt,NULL) != SQLITE_OK)
	{
		raiseError("evaluateLastReadingAvailable", sqlite3_errmsg(dbHandle));
		*maxId = -1;
	}
	else
	{
		// Iterate over all the rows in the resultSet
		*maxId = 0;
		*tableCount = 0;
		while ((rc = SQLStep(stmt)) == SQLITE_ROW)
		{
			nCols = sqlite3_column_count(stmt);

			tableName = (char *)sqlite3_column_text(stmt, 0);
			id = stoi(tableName.substr (tableName.find('_') + 1));

			if (id > *maxId)
				*maxId = id;

			(*tableCount)++;
		}

		sqlite3_finalize(stmt);
	}

}

bool  ReadingsCatalogue::isReadingAvailable() const
{
	if (m_nReadingsAvailable <= 0)
		return false;
	else
		return true;

}

void  ReadingsCatalogue::allocateReadingAvailable()
{
	m_nReadingsAvailable--;
	m_nReadingsUsed++;
}


/**
 * # FIXME_I:
 */
int ReadingsCatalogue::getReadingReference(Connection *connection, const char *asset_code)
{
	sqlite3_stmt *stmt;
	string sql_cmd;
	int rc;
	sqlite3		*dbHandle;

	int readingsId;
	string msg;

	dbHandle = connection->getDbHandle();

	Logger *logger = Logger::getLogger();

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx getReadingReference");
	Logger::getLogger()->setMinLevel("warning");


	auto item = m_AssetReadingCatalogue.find(asset_code);
	if (item != m_AssetReadingCatalogue.end())
	{
		//# An asset already  managed
		readingsId = item->second.first;
	}
	else
	{
		m_mutexAssetReadingCatalogue.lock();

		auto item = m_AssetReadingCatalogue.find(asset_code);
		if (item != m_AssetReadingCatalogue.end())
		{
			readingsId = item->second.first;
		}
		else
		{
			//# Allocate a new block of readings table
			if (! isReadingAvailable () )
			{
				//# FIXME_I
				Logger::getLogger()->setMinLevel("debug");
				Logger::getLogger()->debug("xxx allocate a block of reading tables");
				Logger::getLogger()->setMinLevel("warning");
				createReadingsTablesNewDB();
			}

			// Associate a reading table to the asset
			{
				//# FIXME_I
				Logger::getLogger()->setMinLevel("debug");
				Logger::getLogger()->debug("xxx allocate a new reading table for the asset :%s: ", asset_code);
				Logger::getLogger()->setMinLevel("warning");

				// Associate the asset to the reading_id
				{
					readingsId = getMaxReadingsId() + 1;

					// # FIXME_I:
					auto newItem = make_pair(readingsId,m_dbId);
					auto newMapValue = make_pair(asset_code, newItem);
					m_AssetReadingCatalogue.insert(newMapValue);
				}

				// Allocate the table in the reading catalogue
				{
					sql_cmd =
						"INSERT INTO  " DB_READINGS ".asset_reading_catalogue (table_id, db_id, asset_code) VALUES  ("
						+ to_string(readingsId) + ","
						+ to_string(m_dbId)     + ","
						+ "\"" + asset_code     + "\")";

					rc = SQLexec (dbHandle, sql_cmd.c_str());
					if (rc != SQLITE_OK)
					{
						msg = string(sqlite3_errmsg(dbHandle)) + " asset :" + asset_code + ":";
						raiseError("asset_reading_catalogue update", msg.c_str());
					}
					allocateReadingAvailable();
				}

			}
		}
		m_mutexAssetReadingCatalogue.unlock();
	}

	return (readingsId);

}

int ReadingsCatalogue::getMaxReadingsId()
{
	int maxId = 0, id;

	for (auto &item : m_AssetReadingCatalogue) {

		id = item.second.first;
		if (id > maxId)
				maxId = id;
	}

	return (maxId);
}

int ReadingsCatalogue::getUsedTablesDbId(int dbId)
{
	int count = 0;

	for (auto &item : m_AssetReadingCatalogue) {

		if (item.second.second = dbId)
			count++;
	}

	return (count);
}


string ReadingsCatalogue::getDbName(int dbId)
{
	return (READINGS_DB_NAME_BASE "_" + to_string(dbId));
}

string ReadingsCatalogue::getReadingsName(int tableId)
{
	return (READINGS_TABLE_NAME_BASE "_" + to_string(tableId));
}


string ReadingsCatalogue::getDbNameFromTableId(int tableId)
{
	string dbName;

	for (auto &item : m_AssetReadingCatalogue)
	{

		if (item.second.first == tableId)
		{
			dbName = READINGS_DB_NAME_BASE "_" + to_string(item.second.second);
			break;
		}
	}
	if (dbName == "")
		dbName = READINGS_DB_NAME_BASE "_1";

	return (dbName);
}




int ReadingsCatalogue::SQLexec(sqlite3 *dbHandle, const char *sqlCmd)
{
	int retries = 0, rc;

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx2 SQLexec Startt cmd :%s: ", sqlCmd);
	Logger::getLogger()->setMinLevel("warning");

	do {
		rc = sqlite3_exec(dbHandle, sqlCmd, NULL, NULL, NULL);
		retries++;
		if (rc == SQLITE_LOCKED || rc == SQLITE_BUSY)
		{
			int interval = (retries * RETRY_BACKOFF);
			usleep(interval);	// sleep retries milliseconds
			if (retries > 5) Logger::getLogger()->info("SQLexec - retry %d of %d, rc=%s, DB connection @ %p, slept for %d msecs",
													   retries, MAX_RETRIES, (rc==SQLITE_LOCKED)?"SQLITE_LOCKED":"SQLITE_BUSY", this, interval);
		}
	} while (retries < MAX_RETRIES && (rc == SQLITE_LOCKED || rc == SQLITE_BUSY));

	if (rc == SQLITE_LOCKED)
	{
		Logger::getLogger()->error("SQLexec - Database still locked after maximum retries");
	}
	if (rc == SQLITE_BUSY)
	{
		Logger::getLogger()->error("SQLexec - Database still busy after maximum retries");
	}

	//# FIXME_I
	Logger::getLogger()->setMinLevel("debug");
	Logger::getLogger()->debug("xxx2 SQLexec END");
	Logger::getLogger()->setMinLevel("warning");


	return rc;
}


int ReadingsCatalogue::SQLStep(sqlite3_stmt *statement)
{
	int retries = 0, rc;

	do {
		rc = sqlite3_step(statement);
		retries++;
		if (rc == SQLITE_LOCKED || rc == SQLITE_BUSY)
		{
			int interval = (retries * RETRY_BACKOFF);
			usleep(interval);	// sleep retries milliseconds
			if (retries > 5) Logger::getLogger()->info("SQLStep: retry %d of %d, rc=%s, DB connection @ %p, slept for %d msecs",
													   retries, MAX_RETRIES, (rc==SQLITE_LOCKED)?"SQLITE_LOCKED":"SQLITE_BUSY", this, interval);
		}
	} while (retries < MAX_RETRIES && (rc == SQLITE_LOCKED || rc == SQLITE_BUSY));

	if (rc == SQLITE_LOCKED)
	{
		Logger::getLogger()->error("Database still locked after maximum retries");
	}
	if (rc == SQLITE_BUSY)
	{
		Logger::getLogger()->error("Database still busy after maximum retries");
	}

	return rc;
}

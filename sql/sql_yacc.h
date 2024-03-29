
/* A Bison parser, made by GNU Bison 2.4.1.  */

/* Skeleton interface for Bison's Yacc-like parsers in C
   
      Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002, 2003, 2004, 2005, 2006
   Free Software Foundation, Inc.
   
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.
   
   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */


/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     ABORT_SYM = 258,
     ACCESSIBLE_SYM = 259,
     ACTION = 260,
     ADD = 261,
     ADDDATE_SYM = 262,
     AFTER_SYM = 263,
     AGAINST = 264,
     AGGREGATE_SYM = 265,
     ALGORITHM_SYM = 266,
     ALL = 267,
     ALTER = 268,
     ANALYSE_SYM = 269,
     ANALYZE_SYM = 270,
     AND_AND_SYM = 271,
     AND_SYM = 272,
     ANY_SYM = 273,
     AS = 274,
     ASC = 275,
     ASCII_SYM = 276,
     ASENSITIVE_SYM = 277,
     AT_SYM = 278,
     AUTOEXTEND_SIZE_SYM = 279,
     AUTO_INC = 280,
     AVG_ROW_LENGTH = 281,
     AVG_SYM = 282,
     BACKUP_SYM = 283,
     BEFORE_SYM = 284,
     BEGIN_SYM = 285,
     BETWEEN_SYM = 286,
     BIGINT = 287,
     BINARY = 288,
     BINLOG_SYM = 289,
     BIN_NUM = 290,
     BIT_AND = 291,
     BIT_OR = 292,
     BIT_SYM = 293,
     BIT_XOR = 294,
     BLOB_SYM = 295,
     BLOCK_SYM = 296,
     BOOLEAN_SYM = 297,
     BOOL_SYM = 298,
     BOTH = 299,
     BTREE_SYM = 300,
     BY = 301,
     BYTE_SYM = 302,
     CACHE_SYM = 303,
     CALL_SYM = 304,
     CASCADE = 305,
     CASCADED = 306,
     CASE_SYM = 307,
     CAST_SYM = 308,
     CATALOG_NAME_SYM = 309,
     CHAIN_SYM = 310,
     CHANGE = 311,
     CHANGED = 312,
     CHARSET = 313,
     CHAR_SYM = 314,
     CHECKSUM_SYM = 315,
     CHECK_SYM = 316,
     CIPHER_SYM = 317,
     CLASS_ORIGIN_SYM = 318,
     CLIENT_SYM = 319,
     CLOSE_SYM = 320,
     COALESCE = 321,
     CODE_SYM = 322,
     COLLATE_SYM = 323,
     COLLATION_SYM = 324,
     COLUMNS = 325,
     COLUMN_SYM = 326,
     COLUMN_FORMAT_SYM = 327,
     COLUMN_NAME_SYM = 328,
     COMMENT_SYM = 329,
     COMMITTED_SYM = 330,
     COMMIT_SYM = 331,
     COMPACT_SYM = 332,
     COMPLETION_SYM = 333,
     COMPRESSED_SYM = 334,
     CONCURRENT = 335,
     CONDITION_SYM = 336,
     CONNECTION_SYM = 337,
     CONSISTENT_SYM = 338,
     CONSTRAINT = 339,
     CONSTRAINT_CATALOG_SYM = 340,
     CONSTRAINT_NAME_SYM = 341,
     CONSTRAINT_SCHEMA_SYM = 342,
     CONTAINS_SYM = 343,
     CONTEXT_SYM = 344,
     CONTINUE_SYM = 345,
     CONVERT_SYM = 346,
     COUNT_SYM = 347,
     CPU_SYM = 348,
     CREATE = 349,
     CROSS = 350,
     CUBE_SYM = 351,
     CURDATE = 352,
     CURRENT_SYM = 353,
     CURRENT_USER = 354,
     CURSOR_SYM = 355,
     CURSOR_NAME_SYM = 356,
     CURTIME = 357,
     DATABASE = 358,
     DATABASES = 359,
     DATAFILE_SYM = 360,
     DATA_SYM = 361,
     DATETIME = 362,
     DATE_ADD_INTERVAL = 363,
     DATE_SUB_INTERVAL = 364,
     DATE_SYM = 365,
     DAY_HOUR_SYM = 366,
     DAY_MICROSECOND_SYM = 367,
     DAY_MINUTE_SYM = 368,
     DAY_SECOND_SYM = 369,
     DAY_SYM = 370,
     DEALLOCATE_SYM = 371,
     DECIMAL_NUM = 372,
     DECIMAL_SYM = 373,
     DECLARE_SYM = 374,
     DEFAULT = 375,
     DEFAULT_AUTH_SYM = 376,
     DEFINER_SYM = 377,
     DELAYED_SYM = 378,
     DELAY_KEY_WRITE_SYM = 379,
     DELETE_SYM = 380,
     DESC = 381,
     DESCRIBE = 382,
     DES_KEY_FILE = 383,
     DETERMINISTIC_SYM = 384,
     DIAGNOSTICS_SYM = 385,
     DIRECTORY_SYM = 386,
     DISABLE_SYM = 387,
     DISCARD = 388,
     DISK_SYM = 389,
     DISTINCT = 390,
     DIV_SYM = 391,
     DOUBLE_SYM = 392,
     DO_SYM = 393,
     DROP = 394,
     DUAL_SYM = 395,
     DUMPFILE = 396,
     DUPLICATE_SYM = 397,
     DYNAMIC_SYM = 398,
     EACH_SYM = 399,
     ELSE = 400,
     ELSEIF_SYM = 401,
     ENABLE_SYM = 402,
     ENCLOSED = 403,
     END = 404,
     ENDS_SYM = 405,
     END_OF_INPUT = 406,
     ENGINES_SYM = 407,
     ENGINE_SYM = 408,
     ENUM = 409,
     EQ = 410,
     EQUAL_SYM = 411,
     ERROR_SYM = 412,
     ERRORS = 413,
     ESCAPED = 414,
     ESCAPE_SYM = 415,
     EVENTS_SYM = 416,
     EVENT_SYM = 417,
     EVERY_SYM = 418,
     EXCHANGE_SYM = 419,
     EXECUTE_SYM = 420,
     EXISTS = 421,
     EXIT_SYM = 422,
     EXPANSION_SYM = 423,
     EXPIRE_SYM = 424,
     EXPORT_SYM = 425,
     EXTENDED_SYM = 426,
     EXTENT_SIZE_SYM = 427,
     EXTRACT_SYM = 428,
     FALSE_SYM = 429,
     FAST_SYM = 430,
     FAULTS_SYM = 431,
     FETCH_SYM = 432,
     FILE_SYM = 433,
     FILTER_SYM = 434,
     FIRST_SYM = 435,
     FIXED_SYM = 436,
     FLOAT_NUM = 437,
     FLOAT_SYM = 438,
     FLUSH_SYM = 439,
     FOLLOWS_SYM = 440,
     FORCE_SYM = 441,
     FOREIGN = 442,
     FOR_SYM = 443,
     FORMAT_SYM = 444,
     FOUND_SYM = 445,
     FROM = 446,
     FULL = 447,
     FULLTEXT_SYM = 448,
     FUNCTION_SYM = 449,
     GE = 450,
     GENERAL = 451,
     GEOMETRYCOLLECTION = 452,
     GEOMETRY_SYM = 453,
     GET_FORMAT = 454,
     GET_SYM = 455,
     GLOBAL_SYM = 456,
     GRANT = 457,
     GRANTS = 458,
     GROUP_SYM = 459,
     GROUP_CONCAT_SYM = 460,
     GT_SYM = 461,
     HANDLER_SYM = 462,
     HASH_SYM = 463,
     HAVING = 464,
     HELP_SYM = 465,
     HEX_NUM = 466,
     HIGH_PRIORITY = 467,
     HOST_SYM = 468,
     HOSTS_SYM = 469,
     HOUR_MICROSECOND_SYM = 470,
     HOUR_MINUTE_SYM = 471,
     HOUR_SECOND_SYM = 472,
     HOUR_SYM = 473,
     IDENT = 474,
     IDENTIFIED_SYM = 475,
     IDENT_QUOTED = 476,
     IF = 477,
     IGNORE_SYM = 478,
     IGNORE_SERVER_IDS_SYM = 479,
     IMPORT = 480,
     INDEXES = 481,
     INDEX_SYM = 482,
     INFILE = 483,
     INITIAL_SIZE_SYM = 484,
     INNER_SYM = 485,
     INOUT_SYM = 486,
     INSENSITIVE_SYM = 487,
     INSERT = 488,
     INSERT_METHOD = 489,
     INSTALL_SYM = 490,
     INTERVAL_SYM = 491,
     INTO = 492,
     INT_SYM = 493,
     INVOKER_SYM = 494,
     IN_SYM = 495,
     IO_AFTER_GTIDS = 496,
     IO_BEFORE_GTIDS = 497,
     IO_SYM = 498,
     IPC_SYM = 499,
     IS = 500,
     ISOLATION = 501,
     ISSUER_SYM = 502,
     ITERATE_SYM = 503,
     JOIN_SYM = 504,
     KEYS = 505,
     KEY_BLOCK_SIZE = 506,
     KEY_SYM = 507,
     KILL_SYM = 508,
     LANGUAGE_SYM = 509,
     LAST_SYM = 510,
     LE = 511,
     LEADING = 512,
     LEAVES = 513,
     LEAVE_SYM = 514,
     LEFT = 515,
     LESS_SYM = 516,
     LEVEL_SYM = 517,
     LEX_HOSTNAME = 518,
     LIKE = 519,
     LIMIT = 520,
     LINEAR_SYM = 521,
     LINES = 522,
     LINESTRING = 523,
     LIST_SYM = 524,
     LOAD = 525,
     LOCAL_SYM = 526,
     LOCATOR_SYM = 527,
     LOCKS_SYM = 528,
     LOCK_SYM = 529,
     LOGFILE_SYM = 530,
     LOGS_SYM = 531,
     LONGBLOB = 532,
     LONGTEXT = 533,
     LONG_NUM = 534,
     LONG_SYM = 535,
     LOOP_SYM = 536,
     LOW_PRIORITY = 537,
     LT = 538,
     MASTER_AUTO_POSITION_SYM = 539,
     MASTER_BIND_SYM = 540,
     MASTER_CONNECT_RETRY_SYM = 541,
     MASTER_DELAY_SYM = 542,
     MASTER_HOST_SYM = 543,
     MASTER_LOG_FILE_SYM = 544,
     MASTER_LOG_POS_SYM = 545,
     MASTER_PASSWORD_SYM = 546,
     MASTER_PORT_SYM = 547,
     MASTER_RETRY_COUNT_SYM = 548,
     MASTER_SERVER_ID_SYM = 549,
     MASTER_SSL_CAPATH_SYM = 550,
     MASTER_SSL_CA_SYM = 551,
     MASTER_SSL_CERT_SYM = 552,
     MASTER_SSL_CIPHER_SYM = 553,
     MASTER_SSL_CRL_SYM = 554,
     MASTER_SSL_CRLPATH_SYM = 555,
     MASTER_SSL_KEY_SYM = 556,
     MASTER_SSL_SYM = 557,
     MASTER_SSL_VERIFY_SERVER_CERT_SYM = 558,
     MASTER_SYM = 559,
     MASTER_USER_SYM = 560,
     MASTER_HEARTBEAT_PERIOD_SYM = 561,
     MATCH = 562,
     MAX_CONNECTIONS_PER_HOUR = 563,
     MAX_QUERIES_PER_HOUR = 564,
     MAX_STATEMENT_TIME_SYM = 565,
     MAX_ROWS = 566,
     MAX_SIZE_SYM = 567,
     MAX_SYM = 568,
     MAX_UPDATES_PER_HOUR = 569,
     MAX_USER_CONNECTIONS_SYM = 570,
     MAX_VALUE_SYM = 571,
     MEDIUMBLOB = 572,
     MEDIUMINT = 573,
     MEDIUMTEXT = 574,
     MEDIUM_SYM = 575,
     MEMORY_SYM = 576,
     MERGE_SYM = 577,
     MESSAGE_TEXT_SYM = 578,
     MICROSECOND_SYM = 579,
     MIGRATE_SYM = 580,
     MINUTE_MICROSECOND_SYM = 581,
     MINUTE_SECOND_SYM = 582,
     MINUTE_SYM = 583,
     MIN_ROWS = 584,
     MIN_SYM = 585,
     MODE_SYM = 586,
     MODIFIES_SYM = 587,
     MODIFY_SYM = 588,
     MOD_SYM = 589,
     MONTH_SYM = 590,
     MULTILINESTRING = 591,
     MULTIPOINT = 592,
     MULTIPOLYGON = 593,
     MUTEX_SYM = 594,
     MYSQL_ERRNO_SYM = 595,
     NAMES_SYM = 596,
     NAME_SYM = 597,
     NATIONAL_SYM = 598,
     NATURAL = 599,
     NCHAR_STRING = 600,
     NCHAR_SYM = 601,
     NDBCLUSTER_SYM = 602,
     NE = 603,
     NEG = 604,
     NEVER_SYM = 605,
     NEW_SYM = 606,
     NEXT_SYM = 607,
     NODEGROUP_SYM = 608,
     NONE_SYM = 609,
     NOT2_SYM = 610,
     NOT_SYM = 611,
     NOW_SYM = 612,
     NO_SYM = 613,
     NO_WAIT_SYM = 614,
     NO_WRITE_TO_BINLOG = 615,
     NULL_SYM = 616,
     NUM = 617,
     NUMBER_SYM = 618,
     NUMERIC_SYM = 619,
     NVARCHAR_SYM = 620,
     OFFSET_SYM = 621,
     OLD_PASSWORD = 622,
     ON = 623,
     ONE_SYM = 624,
     ONLY_SYM = 625,
     OPEN_SYM = 626,
     OPTIMIZE = 627,
     OPTIONS_SYM = 628,
     OPTION = 629,
     OPTIONALLY = 630,
     OR2_SYM = 631,
     ORDER_SYM = 632,
     OR_OR_SYM = 633,
     OR_SYM = 634,
     OUTER = 635,
     OUTFILE = 636,
     OUT_SYM = 637,
     OWNER_SYM = 638,
     PACK_KEYS_SYM = 639,
     PAGE_SYM = 640,
     PARAM_MARKER = 641,
     PARSER_SYM = 642,
     PARTIAL = 643,
     PARTITION_SYM = 644,
     PARTITIONS_SYM = 645,
     PARTITIONING_SYM = 646,
     PASSWORD = 647,
     PHASE_SYM = 648,
     PLUGIN_DIR_SYM = 649,
     PLUGIN_SYM = 650,
     PLUGINS_SYM = 651,
     POINT_SYM = 652,
     POLYGON = 653,
     PORT_SYM = 654,
     POSITION_SYM = 655,
     PRECEDES_SYM = 656,
     PRECISION = 657,
     PREPARE_SYM = 658,
     PRESERVE_SYM = 659,
     PREV_SYM = 660,
     PRIMARY_SYM = 661,
     PRIVILEGES = 662,
     PROCEDURE_SYM = 663,
     PROCESS = 664,
     PROCESSLIST_SYM = 665,
     PROFILE_SYM = 666,
     PROFILES_SYM = 667,
     PROXY_SYM = 668,
     PURGE = 669,
     QUARTER_SYM = 670,
     QUERY_SYM = 671,
     QUICK = 672,
     RANGE_SYM = 673,
     READS_SYM = 674,
     READ_ONLY_SYM = 675,
     READ_SYM = 676,
     READ_WRITE_SYM = 677,
     REAL = 678,
     REBUILD_SYM = 679,
     RECOVER_SYM = 680,
     REDOFILE_SYM = 681,
     REDO_BUFFER_SIZE_SYM = 682,
     REDUNDANT_SYM = 683,
     REFERENCES = 684,
     REGEXP = 685,
     RELAY = 686,
     RELAYLOG_SYM = 687,
     RELAY_LOG_FILE_SYM = 688,
     RELAY_LOG_POS_SYM = 689,
     RELAY_THREAD = 690,
     RELEASE_SYM = 691,
     RELOAD = 692,
     REMOVE_SYM = 693,
     RENAME = 694,
     REORGANIZE_SYM = 695,
     REPAIR = 696,
     REPEATABLE_SYM = 697,
     REPEAT_SYM = 698,
     REPLACE = 699,
     REPLICATION = 700,
     REPLICATE_DO_DB = 701,
     REPLICATE_IGNORE_DB = 702,
     REPLICATE_DO_TABLE = 703,
     REPLICATE_IGNORE_TABLE = 704,
     REPLICATE_WILD_DO_TABLE = 705,
     REPLICATE_WILD_IGNORE_TABLE = 706,
     REPLICATE_REWRITE_DB = 707,
     REQUIRE_SYM = 708,
     RESET_SYM = 709,
     RESIGNAL_SYM = 710,
     RESOURCES = 711,
     RESTORE_SYM = 712,
     RESTRICT = 713,
     RESUME_SYM = 714,
     RETURNED_SQLSTATE_SYM = 715,
     RETURNS_SYM = 716,
     RETURN_SYM = 717,
     REVERSE_SYM = 718,
     REVOKE = 719,
     RIGHT = 720,
     ROLLBACK_SYM = 721,
     ROLLUP_SYM = 722,
     ROUTINE_SYM = 723,
     ROWS_SYM = 724,
     ROW_FORMAT_SYM = 725,
     ROW_SYM = 726,
     ROW_COUNT_SYM = 727,
     RTREE_SYM = 728,
     SAVEPOINT_SYM = 729,
     SCHEDULE_SYM = 730,
     SCHEMA_NAME_SYM = 731,
     SECOND_MICROSECOND_SYM = 732,
     SECOND_SYM = 733,
     SECURITY_SYM = 734,
     SELECT_SYM = 735,
     SENSITIVE_SYM = 736,
     SEPARATOR_SYM = 737,
     SERIALIZABLE_SYM = 738,
     SERIAL_SYM = 739,
     SESSION_SYM = 740,
     SERVER_SYM = 741,
     SERVER_OPTIONS = 742,
     SET = 743,
     SET_VAR = 744,
     SHARE_SYM = 745,
     SHIFT_LEFT = 746,
     SHIFT_RIGHT = 747,
     SHOW = 748,
     SHUTDOWN = 749,
     SIGNAL_SYM = 750,
     SIGNED_SYM = 751,
     SIMPLE_SYM = 752,
     SLAVE = 753,
     SLOW = 754,
     SMALLINT = 755,
     SNAPSHOT_SYM = 756,
     SOCKET_SYM = 757,
     SONAME_SYM = 758,
     SOUNDS_SYM = 759,
     SOURCE_SYM = 760,
     SPATIAL_SYM = 761,
     SPECIFIC_SYM = 762,
     SQLEXCEPTION_SYM = 763,
     SQLSTATE_SYM = 764,
     SQLWARNING_SYM = 765,
     SQL_AFTER_GTIDS = 766,
     SQL_AFTER_MTS_GAPS = 767,
     SQL_BEFORE_GTIDS = 768,
     SQL_BIG_RESULT = 769,
     SQL_BUFFER_RESULT = 770,
     SQL_CACHE_SYM = 771,
     SQL_CALC_FOUND_ROWS = 772,
     SQL_NO_CACHE_SYM = 773,
     SQL_SMALL_RESULT = 774,
     SQL_SYM = 775,
     SQL_THREAD = 776,
     SSL_SYM = 777,
     STACKED_SYM = 778,
     STARTING = 779,
     STARTS_SYM = 780,
     START_SYM = 781,
     STATS_AUTO_RECALC_SYM = 782,
     STATS_PERSISTENT_SYM = 783,
     STATS_SAMPLE_PAGES_SYM = 784,
     STATUS_SYM = 785,
     NONBLOCKING_SYM = 786,
     STDDEV_SAMP_SYM = 787,
     STD_SYM = 788,
     STOP_SYM = 789,
     STORAGE_SYM = 790,
     STRAIGHT_JOIN = 791,
     STRING_SYM = 792,
     SUBCLASS_ORIGIN_SYM = 793,
     SUBDATE_SYM = 794,
     SUBJECT_SYM = 795,
     SUBPARTITIONS_SYM = 796,
     SUBPARTITION_SYM = 797,
     SUBSTRING = 798,
     SUM_SYM = 799,
     SUPER_SYM = 800,
     SUSPEND_SYM = 801,
     SWAPS_SYM = 802,
     SWITCHES_SYM = 803,
     SYSDATE = 804,
     TABLES = 805,
     TABLESPACE = 806,
     TABLE_REF_PRIORITY = 807,
     TABLE_SYM = 808,
     TABLE_CHECKSUM_SYM = 809,
     TABLE_NAME_SYM = 810,
     TEMPORARY = 811,
     TEMPTABLE_SYM = 812,
     TERMINATED = 813,
     TEXT_STRING = 814,
     TEXT_SYM = 815,
     THAN_SYM = 816,
     THEN_SYM = 817,
     TIMESTAMP = 818,
     TIMESTAMP_ADD = 819,
     TIMESTAMP_DIFF = 820,
     TIME_SYM = 821,
     TINYBLOB = 822,
     TINYINT = 823,
     TINYTEXT = 824,
     TO_SYM = 825,
     TRAILING = 826,
     TRANSACTION_SYM = 827,
     TRIGGERS_SYM = 828,
     TRIGGER_SYM = 829,
     TRIM = 830,
     TRUE_SYM = 831,
     TRUNCATE_SYM = 832,
     TYPES_SYM = 833,
     TYPE_SYM = 834,
     UDF_RETURNS_SYM = 835,
     ULONGLONG_NUM = 836,
     UNCOMMITTED_SYM = 837,
     UNDEFINED_SYM = 838,
     UNDERSCORE_CHARSET = 839,
     UNDOFILE_SYM = 840,
     UNDO_BUFFER_SIZE_SYM = 841,
     UNDO_SYM = 842,
     UNICODE_SYM = 843,
     UNINSTALL_SYM = 844,
     UNION_SYM = 845,
     UNIQUE_SYM = 846,
     UNKNOWN_SYM = 847,
     UNLOCK_SYM = 848,
     UNSIGNED = 849,
     UNTIL_SYM = 850,
     UPDATE_SYM = 851,
     UPGRADE_SYM = 852,
     USAGE = 853,
     USER = 854,
     USE_FRM = 855,
     USE_SYM = 856,
     USING = 857,
     UTC_DATE_SYM = 858,
     UTC_TIMESTAMP_SYM = 859,
     UTC_TIME_SYM = 860,
     VALUES = 861,
     VALUE_SYM = 862,
     VARBINARY = 863,
     VARCHAR = 864,
     VARIABLES = 865,
     VARIANCE_SYM = 866,
     VARYING = 867,
     VAR_SAMP_SYM = 868,
     VIEW_SYM = 869,
     WAIT_SYM = 870,
     WARNINGS = 871,
     WEEK_SYM = 872,
     WEIGHT_STRING_SYM = 873,
     WHEN_SYM = 874,
     WHERE = 875,
     WHILE_SYM = 876,
     WITH = 877,
     WITH_CUBE_SYM = 878,
     WITH_ROLLUP_SYM = 879,
     WORK_SYM = 880,
     WRAPPER_SYM = 881,
     WRITE_SYM = 882,
     X509_SYM = 883,
     XA_SYM = 884,
     XML_SYM = 885,
     XOR = 886,
     YEAR_MONTH_SYM = 887,
     YEAR_SYM = 888,
     ZEROFILL = 889
   };
#endif
/* Tokens.  */
#define ABORT_SYM 258
#define ACCESSIBLE_SYM 259
#define ACTION 260
#define ADD 261
#define ADDDATE_SYM 262
#define AFTER_SYM 263
#define AGAINST 264
#define AGGREGATE_SYM 265
#define ALGORITHM_SYM 266
#define ALL 267
#define ALTER 268
#define ANALYSE_SYM 269
#define ANALYZE_SYM 270
#define AND_AND_SYM 271
#define AND_SYM 272
#define ANY_SYM 273
#define AS 274
#define ASC 275
#define ASCII_SYM 276
#define ASENSITIVE_SYM 277
#define AT_SYM 278
#define AUTOEXTEND_SIZE_SYM 279
#define AUTO_INC 280
#define AVG_ROW_LENGTH 281
#define AVG_SYM 282
#define BACKUP_SYM 283
#define BEFORE_SYM 284
#define BEGIN_SYM 285
#define BETWEEN_SYM 286
#define BIGINT 287
#define BINARY 288
#define BINLOG_SYM 289
#define BIN_NUM 290
#define BIT_AND 291
#define BIT_OR 292
#define BIT_SYM 293
#define BIT_XOR 294
#define BLOB_SYM 295
#define BLOCK_SYM 296
#define BOOLEAN_SYM 297
#define BOOL_SYM 298
#define BOTH 299
#define BTREE_SYM 300
#define BY 301
#define BYTE_SYM 302
#define CACHE_SYM 303
#define CALL_SYM 304
#define CASCADE 305
#define CASCADED 306
#define CASE_SYM 307
#define CAST_SYM 308
#define CATALOG_NAME_SYM 309
#define CHAIN_SYM 310
#define CHANGE 311
#define CHANGED 312
#define CHARSET 313
#define CHAR_SYM 314
#define CHECKSUM_SYM 315
#define CHECK_SYM 316
#define CIPHER_SYM 317
#define CLASS_ORIGIN_SYM 318
#define CLIENT_SYM 319
#define CLOSE_SYM 320
#define COALESCE 321
#define CODE_SYM 322
#define COLLATE_SYM 323
#define COLLATION_SYM 324
#define COLUMNS 325
#define COLUMN_SYM 326
#define COLUMN_FORMAT_SYM 327
#define COLUMN_NAME_SYM 328
#define COMMENT_SYM 329
#define COMMITTED_SYM 330
#define COMMIT_SYM 331
#define COMPACT_SYM 332
#define COMPLETION_SYM 333
#define COMPRESSED_SYM 334
#define CONCURRENT 335
#define CONDITION_SYM 336
#define CONNECTION_SYM 337
#define CONSISTENT_SYM 338
#define CONSTRAINT 339
#define CONSTRAINT_CATALOG_SYM 340
#define CONSTRAINT_NAME_SYM 341
#define CONSTRAINT_SCHEMA_SYM 342
#define CONTAINS_SYM 343
#define CONTEXT_SYM 344
#define CONTINUE_SYM 345
#define CONVERT_SYM 346
#define COUNT_SYM 347
#define CPU_SYM 348
#define CREATE 349
#define CROSS 350
#define CUBE_SYM 351
#define CURDATE 352
#define CURRENT_SYM 353
#define CURRENT_USER 354
#define CURSOR_SYM 355
#define CURSOR_NAME_SYM 356
#define CURTIME 357
#define DATABASE 358
#define DATABASES 359
#define DATAFILE_SYM 360
#define DATA_SYM 361
#define DATETIME 362
#define DATE_ADD_INTERVAL 363
#define DATE_SUB_INTERVAL 364
#define DATE_SYM 365
#define DAY_HOUR_SYM 366
#define DAY_MICROSECOND_SYM 367
#define DAY_MINUTE_SYM 368
#define DAY_SECOND_SYM 369
#define DAY_SYM 370
#define DEALLOCATE_SYM 371
#define DECIMAL_NUM 372
#define DECIMAL_SYM 373
#define DECLARE_SYM 374
#define DEFAULT 375
#define DEFAULT_AUTH_SYM 376
#define DEFINER_SYM 377
#define DELAYED_SYM 378
#define DELAY_KEY_WRITE_SYM 379
#define DELETE_SYM 380
#define DESC 381
#define DESCRIBE 382
#define DES_KEY_FILE 383
#define DETERMINISTIC_SYM 384
#define DIAGNOSTICS_SYM 385
#define DIRECTORY_SYM 386
#define DISABLE_SYM 387
#define DISCARD 388
#define DISK_SYM 389
#define DISTINCT 390
#define DIV_SYM 391
#define DOUBLE_SYM 392
#define DO_SYM 393
#define DROP 394
#define DUAL_SYM 395
#define DUMPFILE 396
#define DUPLICATE_SYM 397
#define DYNAMIC_SYM 398
#define EACH_SYM 399
#define ELSE 400
#define ELSEIF_SYM 401
#define ENABLE_SYM 402
#define ENCLOSED 403
#define END 404
#define ENDS_SYM 405
#define END_OF_INPUT 406
#define ENGINES_SYM 407
#define ENGINE_SYM 408
#define ENUM 409
#define EQ 410
#define EQUAL_SYM 411
#define ERROR_SYM 412
#define ERRORS 413
#define ESCAPED 414
#define ESCAPE_SYM 415
#define EVENTS_SYM 416
#define EVENT_SYM 417
#define EVERY_SYM 418
#define EXCHANGE_SYM 419
#define EXECUTE_SYM 420
#define EXISTS 421
#define EXIT_SYM 422
#define EXPANSION_SYM 423
#define EXPIRE_SYM 424
#define EXPORT_SYM 425
#define EXTENDED_SYM 426
#define EXTENT_SIZE_SYM 427
#define EXTRACT_SYM 428
#define FALSE_SYM 429
#define FAST_SYM 430
#define FAULTS_SYM 431
#define FETCH_SYM 432
#define FILE_SYM 433
#define FILTER_SYM 434
#define FIRST_SYM 435
#define FIXED_SYM 436
#define FLOAT_NUM 437
#define FLOAT_SYM 438
#define FLUSH_SYM 439
#define FOLLOWS_SYM 440
#define FORCE_SYM 441
#define FOREIGN 442
#define FOR_SYM 443
#define FORMAT_SYM 444
#define FOUND_SYM 445
#define FROM 446
#define FULL 447
#define FULLTEXT_SYM 448
#define FUNCTION_SYM 449
#define GE 450
#define GENERAL 451
#define GEOMETRYCOLLECTION 452
#define GEOMETRY_SYM 453
#define GET_FORMAT 454
#define GET_SYM 455
#define GLOBAL_SYM 456
#define GRANT 457
#define GRANTS 458
#define GROUP_SYM 459
#define GROUP_CONCAT_SYM 460
#define GT_SYM 461
#define HANDLER_SYM 462
#define HASH_SYM 463
#define HAVING 464
#define HELP_SYM 465
#define HEX_NUM 466
#define HIGH_PRIORITY 467
#define HOST_SYM 468
#define HOSTS_SYM 469
#define HOUR_MICROSECOND_SYM 470
#define HOUR_MINUTE_SYM 471
#define HOUR_SECOND_SYM 472
#define HOUR_SYM 473
#define IDENT 474
#define IDENTIFIED_SYM 475
#define IDENT_QUOTED 476
#define IF 477
#define IGNORE_SYM 478
#define IGNORE_SERVER_IDS_SYM 479
#define IMPORT 480
#define INDEXES 481
#define INDEX_SYM 482
#define INFILE 483
#define INITIAL_SIZE_SYM 484
#define INNER_SYM 485
#define INOUT_SYM 486
#define INSENSITIVE_SYM 487
#define INSERT 488
#define INSERT_METHOD 489
#define INSTALL_SYM 490
#define INTERVAL_SYM 491
#define INTO 492
#define INT_SYM 493
#define INVOKER_SYM 494
#define IN_SYM 495
#define IO_AFTER_GTIDS 496
#define IO_BEFORE_GTIDS 497
#define IO_SYM 498
#define IPC_SYM 499
#define IS 500
#define ISOLATION 501
#define ISSUER_SYM 502
#define ITERATE_SYM 503
#define JOIN_SYM 504
#define KEYS 505
#define KEY_BLOCK_SIZE 506
#define KEY_SYM 507
#define KILL_SYM 508
#define LANGUAGE_SYM 509
#define LAST_SYM 510
#define LE 511
#define LEADING 512
#define LEAVES 513
#define LEAVE_SYM 514
#define LEFT 515
#define LESS_SYM 516
#define LEVEL_SYM 517
#define LEX_HOSTNAME 518
#define LIKE 519
#define LIMIT 520
#define LINEAR_SYM 521
#define LINES 522
#define LINESTRING 523
#define LIST_SYM 524
#define LOAD 525
#define LOCAL_SYM 526
#define LOCATOR_SYM 527
#define LOCKS_SYM 528
#define LOCK_SYM 529
#define LOGFILE_SYM 530
#define LOGS_SYM 531
#define LONGBLOB 532
#define LONGTEXT 533
#define LONG_NUM 534
#define LONG_SYM 535
#define LOOP_SYM 536
#define LOW_PRIORITY 537
#define LT 538
#define MASTER_AUTO_POSITION_SYM 539
#define MASTER_BIND_SYM 540
#define MASTER_CONNECT_RETRY_SYM 541
#define MASTER_DELAY_SYM 542
#define MASTER_HOST_SYM 543
#define MASTER_LOG_FILE_SYM 544
#define MASTER_LOG_POS_SYM 545
#define MASTER_PASSWORD_SYM 546
#define MASTER_PORT_SYM 547
#define MASTER_RETRY_COUNT_SYM 548
#define MASTER_SERVER_ID_SYM 549
#define MASTER_SSL_CAPATH_SYM 550
#define MASTER_SSL_CA_SYM 551
#define MASTER_SSL_CERT_SYM 552
#define MASTER_SSL_CIPHER_SYM 553
#define MASTER_SSL_CRL_SYM 554
#define MASTER_SSL_CRLPATH_SYM 555
#define MASTER_SSL_KEY_SYM 556
#define MASTER_SSL_SYM 557
#define MASTER_SSL_VERIFY_SERVER_CERT_SYM 558
#define MASTER_SYM 559
#define MASTER_USER_SYM 560
#define MASTER_HEARTBEAT_PERIOD_SYM 561
#define MATCH 562
#define MAX_CONNECTIONS_PER_HOUR 563
#define MAX_QUERIES_PER_HOUR 564
#define MAX_STATEMENT_TIME_SYM 565
#define MAX_ROWS 566
#define MAX_SIZE_SYM 567
#define MAX_SYM 568
#define MAX_UPDATES_PER_HOUR 569
#define MAX_USER_CONNECTIONS_SYM 570
#define MAX_VALUE_SYM 571
#define MEDIUMBLOB 572
#define MEDIUMINT 573
#define MEDIUMTEXT 574
#define MEDIUM_SYM 575
#define MEMORY_SYM 576
#define MERGE_SYM 577
#define MESSAGE_TEXT_SYM 578
#define MICROSECOND_SYM 579
#define MIGRATE_SYM 580
#define MINUTE_MICROSECOND_SYM 581
#define MINUTE_SECOND_SYM 582
#define MINUTE_SYM 583
#define MIN_ROWS 584
#define MIN_SYM 585
#define MODE_SYM 586
#define MODIFIES_SYM 587
#define MODIFY_SYM 588
#define MOD_SYM 589
#define MONTH_SYM 590
#define MULTILINESTRING 591
#define MULTIPOINT 592
#define MULTIPOLYGON 593
#define MUTEX_SYM 594
#define MYSQL_ERRNO_SYM 595
#define NAMES_SYM 596
#define NAME_SYM 597
#define NATIONAL_SYM 598
#define NATURAL 599
#define NCHAR_STRING 600
#define NCHAR_SYM 601
#define NDBCLUSTER_SYM 602
#define NE 603
#define NEG 604
#define NEVER_SYM 605
#define NEW_SYM 606
#define NEXT_SYM 607
#define NODEGROUP_SYM 608
#define NONE_SYM 609
#define NOT2_SYM 610
#define NOT_SYM 611
#define NOW_SYM 612
#define NO_SYM 613
#define NO_WAIT_SYM 614
#define NO_WRITE_TO_BINLOG 615
#define NULL_SYM 616
#define NUM 617
#define NUMBER_SYM 618
#define NUMERIC_SYM 619
#define NVARCHAR_SYM 620
#define OFFSET_SYM 621
#define OLD_PASSWORD 622
#define ON 623
#define ONE_SYM 624
#define ONLY_SYM 625
#define OPEN_SYM 626
#define OPTIMIZE 627
#define OPTIONS_SYM 628
#define OPTION 629
#define OPTIONALLY 630
#define OR2_SYM 631
#define ORDER_SYM 632
#define OR_OR_SYM 633
#define OR_SYM 634
#define OUTER 635
#define OUTFILE 636
#define OUT_SYM 637
#define OWNER_SYM 638
#define PACK_KEYS_SYM 639
#define PAGE_SYM 640
#define PARAM_MARKER 641
#define PARSER_SYM 642
#define PARTIAL 643
#define PARTITION_SYM 644
#define PARTITIONS_SYM 645
#define PARTITIONING_SYM 646
#define PASSWORD 647
#define PHASE_SYM 648
#define PLUGIN_DIR_SYM 649
#define PLUGIN_SYM 650
#define PLUGINS_SYM 651
#define POINT_SYM 652
#define POLYGON 653
#define PORT_SYM 654
#define POSITION_SYM 655
#define PRECEDES_SYM 656
#define PRECISION 657
#define PREPARE_SYM 658
#define PRESERVE_SYM 659
#define PREV_SYM 660
#define PRIMARY_SYM 661
#define PRIVILEGES 662
#define PROCEDURE_SYM 663
#define PROCESS 664
#define PROCESSLIST_SYM 665
#define PROFILE_SYM 666
#define PROFILES_SYM 667
#define PROXY_SYM 668
#define PURGE 669
#define QUARTER_SYM 670
#define QUERY_SYM 671
#define QUICK 672
#define RANGE_SYM 673
#define READS_SYM 674
#define READ_ONLY_SYM 675
#define READ_SYM 676
#define READ_WRITE_SYM 677
#define REAL 678
#define REBUILD_SYM 679
#define RECOVER_SYM 680
#define REDOFILE_SYM 681
#define REDO_BUFFER_SIZE_SYM 682
#define REDUNDANT_SYM 683
#define REFERENCES 684
#define REGEXP 685
#define RELAY 686
#define RELAYLOG_SYM 687
#define RELAY_LOG_FILE_SYM 688
#define RELAY_LOG_POS_SYM 689
#define RELAY_THREAD 690
#define RELEASE_SYM 691
#define RELOAD 692
#define REMOVE_SYM 693
#define RENAME 694
#define REORGANIZE_SYM 695
#define REPAIR 696
#define REPEATABLE_SYM 697
#define REPEAT_SYM 698
#define REPLACE 699
#define REPLICATION 700
#define REPLICATE_DO_DB 701
#define REPLICATE_IGNORE_DB 702
#define REPLICATE_DO_TABLE 703
#define REPLICATE_IGNORE_TABLE 704
#define REPLICATE_WILD_DO_TABLE 705
#define REPLICATE_WILD_IGNORE_TABLE 706
#define REPLICATE_REWRITE_DB 707
#define REQUIRE_SYM 708
#define RESET_SYM 709
#define RESIGNAL_SYM 710
#define RESOURCES 711
#define RESTORE_SYM 712
#define RESTRICT 713
#define RESUME_SYM 714
#define RETURNED_SQLSTATE_SYM 715
#define RETURNS_SYM 716
#define RETURN_SYM 717
#define REVERSE_SYM 718
#define REVOKE 719
#define RIGHT 720
#define ROLLBACK_SYM 721
#define ROLLUP_SYM 722
#define ROUTINE_SYM 723
#define ROWS_SYM 724
#define ROW_FORMAT_SYM 725
#define ROW_SYM 726
#define ROW_COUNT_SYM 727
#define RTREE_SYM 728
#define SAVEPOINT_SYM 729
#define SCHEDULE_SYM 730
#define SCHEMA_NAME_SYM 731
#define SECOND_MICROSECOND_SYM 732
#define SECOND_SYM 733
#define SECURITY_SYM 734
#define SELECT_SYM 735
#define SENSITIVE_SYM 736
#define SEPARATOR_SYM 737
#define SERIALIZABLE_SYM 738
#define SERIAL_SYM 739
#define SESSION_SYM 740
#define SERVER_SYM 741
#define SERVER_OPTIONS 742
#define SET 743
#define SET_VAR 744
#define SHARE_SYM 745
#define SHIFT_LEFT 746
#define SHIFT_RIGHT 747
#define SHOW 748
#define SHUTDOWN 749
#define SIGNAL_SYM 750
#define SIGNED_SYM 751
#define SIMPLE_SYM 752
#define SLAVE 753
#define SLOW 754
#define SMALLINT 755
#define SNAPSHOT_SYM 756
#define SOCKET_SYM 757
#define SONAME_SYM 758
#define SOUNDS_SYM 759
#define SOURCE_SYM 760
#define SPATIAL_SYM 761
#define SPECIFIC_SYM 762
#define SQLEXCEPTION_SYM 763
#define SQLSTATE_SYM 764
#define SQLWARNING_SYM 765
#define SQL_AFTER_GTIDS 766
#define SQL_AFTER_MTS_GAPS 767
#define SQL_BEFORE_GTIDS 768
#define SQL_BIG_RESULT 769
#define SQL_BUFFER_RESULT 770
#define SQL_CACHE_SYM 771
#define SQL_CALC_FOUND_ROWS 772
#define SQL_NO_CACHE_SYM 773
#define SQL_SMALL_RESULT 774
#define SQL_SYM 775
#define SQL_THREAD 776
#define SSL_SYM 777
#define STACKED_SYM 778
#define STARTING 779
#define STARTS_SYM 780
#define START_SYM 781
#define STATS_AUTO_RECALC_SYM 782
#define STATS_PERSISTENT_SYM 783
#define STATS_SAMPLE_PAGES_SYM 784
#define STATUS_SYM 785
#define NONBLOCKING_SYM 786
#define STDDEV_SAMP_SYM 787
#define STD_SYM 788
#define STOP_SYM 789
#define STORAGE_SYM 790
#define STRAIGHT_JOIN 791
#define STRING_SYM 792
#define SUBCLASS_ORIGIN_SYM 793
#define SUBDATE_SYM 794
#define SUBJECT_SYM 795
#define SUBPARTITIONS_SYM 796
#define SUBPARTITION_SYM 797
#define SUBSTRING 798
#define SUM_SYM 799
#define SUPER_SYM 800
#define SUSPEND_SYM 801
#define SWAPS_SYM 802
#define SWITCHES_SYM 803
#define SYSDATE 804
#define TABLES 805
#define TABLESPACE 806
#define TABLE_REF_PRIORITY 807
#define TABLE_SYM 808
#define TABLE_CHECKSUM_SYM 809
#define TABLE_NAME_SYM 810
#define TEMPORARY 811
#define TEMPTABLE_SYM 812
#define TERMINATED 813
#define TEXT_STRING 814
#define TEXT_SYM 815
#define THAN_SYM 816
#define THEN_SYM 817
#define TIMESTAMP 818
#define TIMESTAMP_ADD 819
#define TIMESTAMP_DIFF 820
#define TIME_SYM 821
#define TINYBLOB 822
#define TINYINT 823
#define TINYTEXT 824
#define TO_SYM 825
#define TRAILING 826
#define TRANSACTION_SYM 827
#define TRIGGERS_SYM 828
#define TRIGGER_SYM 829
#define TRIM 830
#define TRUE_SYM 831
#define TRUNCATE_SYM 832
#define TYPES_SYM 833
#define TYPE_SYM 834
#define UDF_RETURNS_SYM 835
#define ULONGLONG_NUM 836
#define UNCOMMITTED_SYM 837
#define UNDEFINED_SYM 838
#define UNDERSCORE_CHARSET 839
#define UNDOFILE_SYM 840
#define UNDO_BUFFER_SIZE_SYM 841
#define UNDO_SYM 842
#define UNICODE_SYM 843
#define UNINSTALL_SYM 844
#define UNION_SYM 845
#define UNIQUE_SYM 846
#define UNKNOWN_SYM 847
#define UNLOCK_SYM 848
#define UNSIGNED 849
#define UNTIL_SYM 850
#define UPDATE_SYM 851
#define UPGRADE_SYM 852
#define USAGE 853
#define USER 854
#define USE_FRM 855
#define USE_SYM 856
#define USING 857
#define UTC_DATE_SYM 858
#define UTC_TIMESTAMP_SYM 859
#define UTC_TIME_SYM 860
#define VALUES 861
#define VALUE_SYM 862
#define VARBINARY 863
#define VARCHAR 864
#define VARIABLES 865
#define VARIANCE_SYM 866
#define VARYING 867
#define VAR_SAMP_SYM 868
#define VIEW_SYM 869
#define WAIT_SYM 870
#define WARNINGS 871
#define WEEK_SYM 872
#define WEIGHT_STRING_SYM 873
#define WHEN_SYM 874
#define WHERE 875
#define WHILE_SYM 876
#define WITH 877
#define WITH_CUBE_SYM 878
#define WITH_ROLLUP_SYM 879
#define WORK_SYM 880
#define WRAPPER_SYM 881
#define WRITE_SYM 882
#define X509_SYM 883
#define XA_SYM 884
#define XML_SYM 885
#define XOR 886
#define YEAR_MONTH_SYM 887
#define YEAR_SYM 888
#define ZEROFILL 889




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
{

/* Line 1676 of yacc.c  */
#line 886 "/export/home/pb2/build/sb_0-11877675-1395933999.37/mysql-trunk-wl7696-export-6487534_gpl/sql/sql_yacc.yy"

  int  num;
  ulong ulong_num;
  ulonglong ulonglong_number;
  longlong longlong_number;
  LEX_STRING lex_str;
  LEX_STRING *lex_str_ptr;
  LEX_SYMBOL symbol;
  Table_ident *table;
  char *simple_string;
  Item *item;
  Item_num *item_num;
  List<Item> *item_list;
  List<String> *string_list;
  String *string;
  Key_part_spec *key_part;
  TABLE_LIST *table_list;
  udf_func *udf;
  LEX_USER *lex_user;
  struct sys_var_with_base variable;
  enum enum_var_type var_type;
  Key::Keytype key_type;
  enum ha_key_alg key_alg;
  handlerton *db_type;
  enum row_type row_type;
  enum ha_rkey_function ha_rkey_mode;
  enum enum_ha_read_modes ha_read_mode;
  enum enum_tx_isolation tx_isolation;
  enum Cast_target cast_type;
  enum Item_udftype udf_type;
  const CHARSET_INFO *charset;
  thr_lock_type lock_type;
  interval_type interval, interval_time_st;
  timestamp_type date_time_type;
  st_select_lex *select_lex;
  chooser_compare_func_creator boolfunc2creator;
  class sp_condition_value *spcondvalue;
  struct { int vars, conds, hndlrs, curs; } spblock;
  sp_name *spname;
  LEX *lex;
  sp_head *sphead;
  struct p_elem_val *p_elem_value;
  enum index_hint_type index_hint;
  enum enum_filetype filetype;
  enum Foreign_key::fk_option m_fk_option;
  enum enum_yes_no_unknown m_yes_no_unk;
  enum_condition_item_name da_condition_item_name;
  Diagnostics_information::Which_area diag_area;
  Diagnostics_information *diag_info;
  Statement_information_item *stmt_info_item;
  Statement_information_item::Name stmt_info_item_name;
  List<Statement_information_item> *stmt_info_list;
  Condition_information_item *cond_info_item;
  Condition_information_item::Name cond_info_item_name;
  List<Condition_information_item> *cond_info_list;
  bool is_not_empty;
  Set_signal_information *signal_item_list;
  enum enum_trigger_order_type trigger_action_order_type;
  struct
  {
    enum enum_trigger_order_type ordering_clause;
    LEX_STRING anchor_trigger_name;
  } trg_characteristics;
  struct
  {
    bool set_password_expire_flag;    /* true if password expires */
    bool use_default_password_expiry; /* true if password_lifetime is NULL*/
    uint16 expire_after_days;
  } user_password_expiration;



/* Line 1676 of yacc.c  */
#line 1393 "/export/home/pb2/build/sb_0-11877675-1395933999.37/dist_GPL/sql/sql_yacc.h"
} YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
#endif



#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
} YYLTYPE;
# define yyltype YYLTYPE /* obsolescent; will be withdrawn */
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif




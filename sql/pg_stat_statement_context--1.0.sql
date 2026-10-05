/* pg_stat_statement_context--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_statement_context" to load this file. \quit

-- SQL objects (stats SRF, views, _info(), _reset(), _extract()) are added by
-- later tasks; see DESIGN.md §7.

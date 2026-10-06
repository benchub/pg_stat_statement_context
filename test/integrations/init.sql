-- Init script of the PostgreSQL container of scripts/test-integrations.sh.
-- The extensions live in "postgres", where the exporters connect; the
-- workload runs as role "app" in database "shop", so the exported series
-- also prove that the views are cluster-wide and that pssc_monitor can see
-- another role's tags.
CREATE EXTENSION pg_stat_statements;
CREATE EXTENSION pg_stat_statement_context;

CREATE ROLE app LOGIN PASSWORD 'app';
CREATE DATABASE shop OWNER app;

\connect shop app

CREATE TABLE users (id int PRIMARY KEY, name text, orders int NOT NULL DEFAULT 0);
INSERT INTO users SELECT g, 'user' || g FROM generate_series(1, 100) g;
CREATE TABLE posts (id int PRIMARY KEY, user_id int, title text);
INSERT INTO posts SELECT g, g % 100 + 1, 'post ' || g FROM generate_series(1, 1000) g;
CREATE TABLE orders (id bigserial PRIMARY KEY, user_id int, created timestamptz DEFAULT now());

-- Three nested statements per call: with track = all they are recorded with
-- toplevel = false and inherit the caller's tags (nested_tags = inherit).
CREATE FUNCTION create_order(uid int) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	new_id bigint;
BEGIN
	INSERT INTO orders (user_id) VALUES (uid) RETURNING id INTO new_id;
	UPDATE users SET orders = orders + 1 WHERE id = uid;
	PERFORM count(*) FROM orders WHERE user_id = uid;
	RETURN new_id;
END
$$;

\connect postgres postgres

// Repro client for pgx v5: does the comment seen at Parse time go stale?
//
// Each call sends `SELECT $1::text AS want /*ctx=cN*/` where $1 = 'cN', so the
// server log can compare the comment in the executed statement's source text
// with the context intended for this call (see ../lib/check_log.py).
//
// Usage: go run . -mode <mode> [-clients N]   (DSN from $DSN)
//
//	default         pgx default: QueryExecModeCacheStatement (named, LRU cache keyed by SQL text)
//	cache_describe  QueryExecModeCacheDescribe (unnamed Parse each call, cached description)
//	describe_exec   QueryExecModeDescribeExec
//	exec            QueryExecModeExec
//	simple          QueryExecModeSimpleProtocol (client-side interpolation)
//	app_prepared    control: application calls conn.Prepare once (with ctx=c0)
//	                and reuses the statement name for every context -> must be stale
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"

	"github.com/jackc/pgx/v5"
)

// 7×c0, 7×c1, 7×c2 (crosses every driver's prepare threshold), then interleaved.
func workload() []string {
	var w []string
	for c := 0; c < 3; c++ {
		for i := 0; i < 7; i++ {
			w = append(w, fmt.Sprintf("c%d", c))
		}
	}
	for i := 0; i < 9; i++ {
		w = append(w, fmt.Sprintf("c%d", i%3))
	}
	return w
}

func main() {
	mode := flag.String("mode", "default", "exec mode")
	clients := flag.Int("clients", 1, "client connections used round-robin")
	flag.Parse()
	ctx := context.Background()

	cfg, err := pgx.ParseConfig(os.Getenv("DSN"))
	if err != nil {
		log.Fatal(err)
	}
	switch *mode {
	case "default", "app_prepared":
		// leave pgx defaults untouched
	case "cache_describe":
		cfg.DefaultQueryExecMode = pgx.QueryExecModeCacheDescribe
	case "describe_exec":
		cfg.DefaultQueryExecMode = pgx.QueryExecModeDescribeExec
	case "exec":
		cfg.DefaultQueryExecMode = pgx.QueryExecModeExec
	case "simple":
		cfg.DefaultQueryExecMode = pgx.QueryExecModeSimpleProtocol
	default:
		log.Fatalf("unknown mode %q", *mode)
	}
	log.Printf("pgx mode=%s DefaultQueryExecMode=%v clients=%d", *mode, cfg.DefaultQueryExecMode, *clients)

	conns := make([]*pgx.Conn, *clients)
	for i := range conns {
		c := cfg.Copy()
		c.RuntimeParams["application_name"] = fmt.Sprintf("repro-%d", i)
		if conns[i], err = pgx.ConnectConfig(ctx, c); err != nil {
			log.Fatal(err)
		}
		defer conns[i].Close(ctx)
	}

	if *mode == "app_prepared" {
		for _, c := range conns {
			if _, err := c.Prepare(ctx, "app_stmt", "SELECT $1::text AS want /*ctx=c0*/"); err != nil {
				log.Fatal(err)
			}
		}
	}

	for i, want := range workload() {
		c := conns[i%len(conns)]
		sql := fmt.Sprintf("SELECT $1::text AS want /*ctx=%s*/", want)
		if *mode == "app_prepared" {
			sql = "app_stmt"
		}
		var got string
		if err := c.QueryRow(ctx, sql, want).Scan(&got); err != nil {
			log.Fatalf("call %d (%s): %v", i, want, err)
		}
		if got != want {
			log.Fatalf("call %d: got %q want %q", i, got, want)
		}
	}
	log.Printf("done: %d calls", len(workload()))
}

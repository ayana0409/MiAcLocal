package main

import (
	"context"
	"errors"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
	_ "time/tzdata"

	"miacstats/internal/config"
	"miacstats/internal/rollup"
	"miacstats/internal/turso"
	"miacstats/internal/web"
)

func main() {
	cfg := config.Load()

	// Check if invoked as a standalone CLI command: miacstats rollup [--force] [--dry-run]
	if len(os.Args) > 1 && os.Args[1] == "rollup" {
		runRollupCLI(cfg)
		return
	}

	var store turso.Store
	if cfg.TursoURL != "" && cfg.TursoToken != "" {
		client := turso.NewClient(cfg.TursoURL, cfg.TursoToken)
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		if err := client.EnsureSchema(ctx); err != nil {
			log.Printf("warning: schema init failed (continuing): %v", err)
		}
		cancel()
		store = client
	} else if !cfg.DemoMode {
		log.Println("warning: TURSO_DATABASE_URL/TURSO_AUTH_TOKEN not set; starting with empty store unless DEMO_MODE=true")
	}

	srvHandler, err := web.NewServer(cfg, store)
	if err != nil {
		log.Fatalf("init web server: %v", err)
	}

	httpServer := &http.Server{
		Addr:              ":" + cfg.Port,
		Handler:           srvHandler,
		ReadHeaderTimeout: 5 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	rootCtx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	go func() {
		log.Printf("MiAcStats listening on :%s (tz=%s, demo=%v, sso=%v)", cfg.Port, cfg.Timezone, cfg.DemoMode, cfg.SSOEnabled)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("listen failed: %v", err)
		}
	}()

	// If Turso client is configured and rollup enabled, run rollup in the background after server is up
	if backend, ok := store.(rollup.Backend); ok && cfg.RollupEnabled {
		go func() {
			time.Sleep(1 * time.Second)
			r := rollup.NewRunner(cfg, backend)
			if err := r.Run(rootCtx, false); err != nil && !errors.Is(err, context.Canceled) {
				log.Printf("rollup background error: %v", err)
			}
		}()
	}

	<-rootCtx.Done()

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(shutdownCtx)
}

func runRollupCLI(cfg config.Config) {
	force := false
	for _, arg := range os.Args[2:] {
		switch arg {
		case "--force", "-f":
			force = true
		case "--dry-run":
			cfg.RollupDryRun = true
		}
	}

	if cfg.TursoURL == "" || cfg.TursoToken == "" {
		log.Fatal("rollup: TURSO_DATABASE_URL and TURSO_AUTH_TOKEN must be configured")
	}

	client := turso.NewClient(cfg.TursoURL, cfg.TursoToken)
	initCtx, cancelInit := context.WithTimeout(context.Background(), 15*time.Second)
	if err := client.EnsureSchema(initCtx); err != nil {
		cancelInit()
		log.Fatalf("rollup: schema init failed: %v", err)
	}
	cancelInit()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	runner := rollup.NewRunner(cfg, client)
	log.Printf("rollup: starting standalone run (force=%v, dry_run=%v)...", force, cfg.RollupDryRun)
	if err := runner.Run(ctx, force); err != nil {
		log.Fatalf("rollup: execution failed: %v", err)
	}
	log.Println("rollup: standalone run finished")
}



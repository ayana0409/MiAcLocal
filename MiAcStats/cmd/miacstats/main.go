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
	"miacstats/internal/turso"
	"miacstats/internal/web"
)

func main() {
	cfg := config.Load()

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

	go func() {
		log.Printf("MiAcStats listening on :%s (tz=%s, demo=%v, sso=%v)", cfg.Port, cfg.Timezone, cfg.DemoMode, cfg.SSOEnabled)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("listen failed: %v", err)
		}
	}()

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	<-stop

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(shutdownCtx)
}


// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 Intel Corporation
// Copyright 2022-present Open Networking Foundation

package main

import (
	"context"
	"errors"
	"flag"
	"net/http"
	"os"
	"os/signal"
	"syscall"

	log "github.com/sirupsen/logrus"
)

var (
	configPath = flag.String("config", "../../bess/bessctl/conf/upf.json", "path to upf config")
	simulate   = simModeDisable
	pfcpsim    = flag.Bool("pfcpsim", false, "simulate PFCP")
	// niccache   = flag.Bool("niccache", false, "cache to NIC")
	// nicdelete  = flag.Bool("nicdelete", false, "delete cache on NIC")
	// nicclear   = flag.Bool("nicclear", false, "clear all cache on NIC")
)

func init() {
	flag.Var(&simulate, "simulate", "create|modify|delete|clear|create_continue simulated sessions")
	// Set up logger
	log.SetReportCaller(true)
	log.SetFormatter(&log.TextFormatter{
		FullTimestamp: true,
	})
}

func main() {
	// cmdline args
	flag.Parse()

	// Read and parse json startup file.
	conf, err := LoadConfigFile(*configPath)
	if err != nil {
		log.Fatalln("Error reading conf file:", err)
	}

	log.SetLevel(conf.LogLevel)

	log.Infof("%+v", conf)

	var fp fastPath
	if conf.EnableP4rt {
		fp = &UP4{}
	} else {
		fp = &bess{}
	}

	// if *niccache {
	// 	nicCache(&conf.SimInfo)
	// 	return
	// }
	// if *nicdelete {
	// 	nicDelete(&conf.SimInfo)
	// 	return
	// }
	// if *nicclear {
	// 	nicClear(&conf.SimInfo)
	// 	return
	// }

	upf := NewUPF(&conf, fp)

	if *pfcpsim {
		pfcpSim()
		return
	}

	if simulate.enable() {
		upf.sim(simulate, &conf.SimInfo)
		if !simulate.keepGoing() {
			return
		}
	}

	setupConfigHandler(upf)

	httpPort := "8080"
	if conf.CPIface.HTTPPort != "" {
		httpPort = conf.CPIface.HTTPPort
	}

	httpSrv := &http.Server{Addr: ":" + httpPort, Handler: nil}

	go func() {
		if err := httpSrv.ListenAndServe(); err != nil && errors.Is(err, http.ErrServerClosed) {
			log.Fatalln("http server failed", err)
		}

		log.Infoln("http server closed")
	}()

	ctx, cancel := context.WithCancel(context.Background())

	node := NewPFCPNode(ctx, upf)
	go node.Serve()

	setupProm(upf, node)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt)
	signal.Notify(sig, syscall.SIGTERM)
	<-sig

	cancel()

	// Wait for node shutdown before http shutdown
	node.Done()

	if err := httpSrv.Shutdown(context.Background()); err != nil {
		log.Errorln("Failed to shutdown http:", err)
	}

	upf.exit()
}

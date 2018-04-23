#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <errno.h>
#include <sys/queue.h>

#include <rte_common.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_sched.h>
#include <cmdline_parse.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_eal.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>

#include <rte_port_ethdev.h>
#include <rte_port_ring.h>
#include <rte_pipeline.h>
#include <signal.h>
#include <unistd.h>


#define APP_PARAM_NAME_SIZE                      256 //the string size for storing the name
#define RTE_MAX_ETHPORTS                         32  //the maximum possible ports

#ifdef RTE_PORT_IN_BURST_SIZE_MAX 
#undef RTE_PORT_IN_BURST_SIZE_MAX
#define RTE_PORT_IN_BURST_SIZE_MAX               64 //the busrt size of the RX queue
#endif

#ifdef RTE_MAX_LCORE
#undef RTE_MAX_LCORE
#define RTE_MAX_LCORE                            12  //the maximum possible cores
#endif
//==========================================================================================================
#define PORT_MASK                                0x01 //to choose the port you need, only 1 port in this example
//==========================================================================================================
struct app_mempool_params {
	uint32_t buffer_size;
	uint32_t pool_size;
	uint32_t cache_size;
};
//these settings are used for memory pool allocation, they are pretty standard. Please don't modify them unless you know what you are doing
static const struct app_mempool_params mempool_params_default = { 
	.buffer_size = 2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM,
	.pool_size = 16 * 1024,
	.cache_size = 256,
};
//==========================================================================================================
struct app_link_params { 
	uint32_t promisc;
	uint64_t mac_addr; 
	struct rte_eth_conf conf;
};

static const struct app_link_params link_params_default = {
	.promisc = 1,
	.mac_addr = 0,
	.conf = {
		.link_speeds = 0,
		.rxmode = {
			.mq_mode = ETH_MQ_RX_RSS, /*****setting up RSS**********/

			.header_split   = 0, // Header split 
			.hw_ip_checksum = 0, // IP checksum offload 
			.hw_vlan_filter = 0, // VLAN filtering 
			.hw_vlan_strip  = 0, // VLAN strip 
			.hw_vlan_extend = 0, // Extended VLAN 
			.jumbo_frame    = 0, // Jumbo frame support 
			.hw_strip_crc   = 0, // CRC strip by HW 
			.enable_scatter = 0, // Scattered packets RX handler 

			.max_rx_pkt_len = 9000, // Jumbo frame max packet len 
			.split_hdr_size = 0, // Header split buffer size 
		},
		.rx_adv_conf = {
			.rss_conf = {
				.rss_key = NULL,
				.rss_key_len = 40,
				.rss_hf = ETH_RSS_IPV4, /******IPV4 packets*******/
			},
		},
		.txmode = {
			.mq_mode = ETH_MQ_TX_NONE,
		},
		.lpbk_mode = 0,
	},
};
//==========================================================================================================
struct app_pktq_hwq_in_params {
	uint32_t size; //the number of ring descriptor of each RX queue
	struct rte_eth_rxconf conf;
};
//these settings are used for setting up the RX queue, they are pretty standard. Please don't modify them unless you know what you are doing
static const struct app_pktq_hwq_in_params default_hwq_in_params = {
	.size = 512, 
	.conf = {
		.rx_thresh = {
				.pthresh = 8,
				.hthresh = 8,
				.wthresh = 4,
		},
		.rx_free_thresh = 64,
		.rx_drop_en = 0,
		.rx_deferred_start = 0,
	}
};
//==========================================================================================================

static void app_init_eal(int argc, char **argv)
{
	int status;
	status = rte_eal_init(argc, argv);
	if (status < 0) { rte_panic("EAL init error\n"); }
}
//get the socket ID for the port ID
static inline int app_get_cpu_socket_id(uint32_t pmd_id)
{
	int status = rte_eth_dev_socket_id(pmd_id);

	return (status != SOCKET_ID_ANY) ? status : 0;
}

static void app_init_link()
{
	int status;
	uint32_t pmd_id;
	for (pmd_id = 0; pmd_id < RTE_MAX_ETHPORTS; pmd_id++)    //iterate through all the possible ports
	{
		if ((PORT_MASK & (1LLU << pmd_id)) == 0) {continue;} //check if the port is needed
		
		// SOCKET ID, get the socket ID for each port in the NUMA, for the best allocation of the memory pool 
		int socket_id = app_get_cpu_socket_id(pmd_id);
		
		/*********************** creating mempool for each of the 3 queues **************************************/

		char name1[128]; sprintf(name01, "MEMPOOL1%u", pmd_id);
		char name2[128]; sprintf(name02, "MEMPOOL2%u", pmd_id);
		char name3[128]; sprintf(name03, "MEMPOOL3%u", pmd_id);

		struct rte_mempool * mempool1;
		struct rte_mempool * mempool2;
		struct rte_mempool * mempool3;

		mempool1 = rte_mempool_create(
								name01,
								mempool_params_default.pool_size,
								mempool_params_default.buffer_size,
								mempool_params_default.cache_size,
								sizeof(struct rte_pktmbuf_pool_private),
								rte_pktmbuf_pool_init, NULL,
								rte_pktmbuf_init, NULL,
								socket_id,
								0 
							);
		
		if(mempool1 == NULL) { printf("Error, can not create mempool for dev %u \n", pmd_id); exit(1); }

		mempool2 = rte_mempool_create(
								name02,
								mempool_params_default.pool_size,
								mempool_params_default.buffer_size,
								mempool_params_default.cache_size,
								sizeof(struct rte_pktmbuf_pool_private),
								rte_pktmbuf_pool_init, NULL,
								rte_pktmbuf_init, NULL,
								socket_id,
								0 
							);
		
		if(mempool2 == NULL) { printf("Error, can not create mempool for dev %u \n", pmd_id); exit(1); }

		mempool3 = rte_mempool_create(
								name03,
								mempool_params_default.pool_size,
								mempool_params_default.buffer_size,
								mempool_params_default.cache_size,
								sizeof(struct rte_pktmbuf_pool_private),
								rte_pktmbuf_pool_init, NULL,
								rte_pktmbuf_init, NULL,
								socket_id,
								0 
							);
		
		if(mempool3 == NULL) { printf("Error, can not create mempool for dev %u \n", pmd_id); exit(1); }
		

		

		
		// Port, each port has 3 RX queue and 0 TX queue
		struct app_link_params link_temp; 
		memcpy(&link_temp, &link_params_default, sizeof(struct app_link_params));
		status = rte_eth_dev_configure(pmd_id, 3, 0, &link_temp.conf); /************** 3 RX queues**************/
		if (status < 0) { printf("Error, can not init dev %u\n", pmd_id); exit(1); }
		
		//one way to get the corresponding mac address for each port
		rte_eth_macaddr_get(pmd_id, (struct ether_addr *) &link_temp.mac_addr);
		
		//enable promiscuous mode, which means this port will receive all the packets
		if (link_temp.promisc) { rte_eth_promiscuous_enable(pmd_id); }

		// RXQ 
		printf("########## Setting UP RXQ For PORT %u ########## \n", pmd_id);
		status = rte_eth_rx_queue_setup(
										pmd_id,
										0,/********queue id********/
										default_hwq_in_params.size,
										socket_id,
										&default_hwq_in_params.conf,
										mempool1 
									);
		if (status < 0) { printf("Error, can not set up queue for dev %u \n", pmd_id);  exit(1); }

		status = rte_eth_rx_queue_setup(
										pmd_id,
										1, /***********queue id************/
										default_hwq_in_params.size,
										socket_id,
										&default_hwq_in_params.conf,
										mempool2 
									);
		if (status < 0) { printf("Error, can not set up queue for dev %u \n", pmd_id);  exit(1); }

		status = rte_eth_rx_queue_setup(
										pmd_id,
										2, /********queue id*********/
										default_hwq_in_params.size,
										socket_id,
										&default_hwq_in_params.conf,
										mempool3
									);
		if (status < 0) { printf("Error, can not set up queue for dev %u \n", pmd_id);  exit(1); }

		
		// LINK START
		status = rte_eth_dev_start(pmd_id);
		if (status < 0) { printf("Error, can not start dev %u \n", pmd_id);  exit(1); }

	}
}



//
int app_init(int argc, char **argv)
{
	app_init_eal(argc, argv);
	app_init_link();
	
	return 0;
}
//==========================================================================================================
struct sniff_ethernet 
{
        u_char  dmac[6];    // destination host address 
        u_char  smac[6];    // source host address 
        u_short ether_type; //  
};
//==========================================================================================================
int app_thread(void *arg)
{
	uint32_t lcore_id = rte_lcore_id();
	uint32_t master_core_id = rte_get_master_lcore();
	uint32_t i; 
	int status;
	struct rte_mbuf *pkts[RTE_PORT_IN_BURST_SIZE_MAX]; //the pointer array that will store the pointer to each received packet
	uint32_t n_pkts; //the number of received packets during one burst
	//
	if(lcore_id == 0)
	{
		printf("Hello from master core 0 !\n", lcore_id);
		 
		while(1)
		{
			uint32_t total_time_in_sec = 10; //for report, in second
			uint64_t p_ticks = total_time_in_sec * rte_get_tsc_hz(); //for report, calculate the total CPU cycles 
			uint64_t p_start = rte_get_tsc_cycles(); //get the current CPU cycle
			uint32_t total_pkts = 0; //for statistics
			while(rte_get_tsc_cycles() - p_start < p_ticks)
			{
		
				n_pkts = rte_eth_rx_burst(0, 0, pkts, RTE_PORT_IN_BURST_SIZE_MAX);  
				if(unlikely(n_pkts == 0)) {continue;} //if no packet received, then start the next try
				total_pkts += n_pkts;
				
				//retrieving the data from each packet
				for(i=0; i<n_pkts; i++)
				{
					//pretty standard way to get the pointer which points to the packet data
					uint8_t* packet = rte_pktmbuf_mtod(pkts[i], uint8_t*); 
					
					//Pease modify the following lines 246-252 according to what you want to do on each packet
					//===============================================================================================================
					//print out the ethertype if it is not the standard IPV4 packets, https://en.wikipedia.org/wiki/EtherType========
					struct sniff_ethernet *ethernet = (struct sniff_ethernet*) packet;//=============================================
					if(ntohs(ethernet->ether_type) != 0x0800)//======================================================================
					{//==============================================================================================================
						//printf("The ether_type of the packet is %x \n", ntohs(ethernet->ether_type));//==============================
					}//==============================================================================================================
				}
				
				//free the packets, this is must-do, otherwise the memory pool will be full, and no more packets can be received
				for(i=0; i<n_pkts; i++)
				{
					rte_pktmbuf_free(pkts[i]);
				}
			}
			printf("lcore 0, received %u packets in %u seconds.\n",total_pkts, total_time_in_sec);
		}

	
	}
	else if(lcore_id == 1)
	{
		printf("Hello from master core 1 !\n", lcore_id);
		 
		while(1)
		{
			uint32_t total_time_in_sec = 10; //for report, in second
			uint64_t p_ticks = total_time_in_sec * rte_get_tsc_hz(); //for report, calculate the total CPU cycles 
			uint64_t p_start = rte_get_tsc_cycles(); //get the current CPU cycle
			uint32_t total_pkts = 0; //for statistics
			while(rte_get_tsc_cycles() - p_start < p_ticks)
			{
				//only 1 port, only 1 queue
				n_pkts = rte_eth_rx_burst(0, 1, pkts, RTE_PORT_IN_BURST_SIZE_MAX); //trying to receive packts 
				if(unlikely(n_pkts == 0)) {continue;} //if no packet received, then start the next try
				total_pkts += n_pkts;
				
				//retrieving the data from each packet
				for(i=0; i<n_pkts; i++)
				{
					//pretty standard way to get the pointer which points to the packet data
					uint8_t* packet = rte_pktmbuf_mtod(pkts[i], uint8_t*); 
					
					//Pease modify the following lines 246-252 according to what you want to do on each packet
					//===============================================================================================================
					//print out the ethertype if it is not the standard IPV4 packets, https://en.wikipedia.org/wiki/EtherType========
					struct sniff_ethernet *ethernet = (struct sniff_ethernet*) packet;//=============================================
					if(ntohs(ethernet->ether_type) != 0x0800)//======================================================================
					{//==============================================================================================================
						//printf("The ether_type of the packet is %x \n", ntohs(ethernet->ether_type));//==============================
					}//==============================================================================================================
				}
				
				//free the packets, this is must-do, otherwise the memory pool will be full, and no more packets can be received
				for(i=0; i<n_pkts; i++)
				{
					rte_pktmbuf_free(pkts[i]);
				}
			}
			printf("lcore 1, received %u packets in %u seconds.\n", total_pkts, total_time_in_sec);
		}			
	}		
	else if(lcore_id == 2)
	{
		printf("Hello from master core 2 !\n", lcore_id);
		 
		while(1)
		{
			uint32_t total_time_in_sec = 10; //for report, in second
			uint64_t p_ticks = total_time_in_sec * rte_get_tsc_hz(); //for report, calculate the total CPU cycles 
			uint64_t p_start = rte_get_tsc_cycles(); //get the current CPU cycle
			uint32_t total_pkts = 0; //for statistics
			while(rte_get_tsc_cycles() - p_start < p_ticks)
			{
				//only 1 port, only 1 queue
				n_pkts = rte_eth_rx_burst(0, 2, pkts, RTE_PORT_IN_BURST_SIZE_MAX); //trying to receive packts 
				if(unlikely(n_pkts == 0)) {continue;} //if no packet received, then start the next try
				total_pkts += n_pkts;
				
				//retrieving the data from each packet
				for(i=0; i<n_pkts; i++)
				{
					//pretty standard way to get the pointer which points to the packet data
					uint8_t* packet = rte_pktmbuf_mtod(pkts[i], uint8_t*); 
					
					//Pease modify the following lines 246-252 according to what you want to do on each packet
					//===============================================================================================================
					//print out the ethertype if it is not the standard IPV4 packets, https://en.wikipedia.org/wiki/EtherType========
					struct sniff_ethernet *ethernet = (struct sniff_ethernet*) packet;//=============================================
					if(ntohs(ethernet->ether_type) != 0x0800)//======================================================================
					{//==============================================================================================================
						printf("The ether_type of the packet is %x \n", ntohs(ethernet->ether_type));//==============================
					}//==============================================================================================================
				}
				
				//free the packets, this is must-do, otherwise the memory pool will be full, and no more packets can be received
				for(i=0; i<n_pkts; i++)
				{
					rte_pktmbuf_free(pkts[i]);
				}
			}
			printf("lcore 2, received %u packets in %u seconds.\n",total_pkts, total_time_in_sec);
		}			
			
	   }	
			
	return 0;	
}
//==========================================================================================================
int main(int argc, char **argv)
{
	app_init(argc, argv);
	rte_eal_mp_remote_launch(app_thread, NULL, CALL_MASTER);
}



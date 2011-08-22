#include "examples/examples.h"
#include "kernel/disk-table.h"

using namespace dsm;
DEFINE_int32(tnum_nodes, 10000, "");
DEFINE_bool(tdump_output, false, "");

static int NUM_WORKERS = 0;
static TypedGlobalTable<int, double>* distance_map;
static RecordTable<PathNode>* nodes_record;
static TypedGlobalTable<int, vector<double> >* nodes;

namespace dsm{
	template <> struct Marshal<vector<double> > : MarshalBase {
		static void marshal(const vector<double>& t, string *out) {
			int i;
			double j;
			int len = t.size();
			out->append((char*)&len,sizeof(int));
			for(i = 0; i < len; i++) {
				j = t[i];
				out->append((char*)&j,sizeof(double));
			}
		}
		static void unmarshal(const StringPiece &s, vector<double>* t) {
			int i;
			double j;
			int len;
			memcpy(&len,s.data,sizeof(int));
			if (len < 0)
				LOG(FATAL) << "Unmarshalled vector of size < 0" << endl;
			t->clear();
			for(i = 0; i < len; i++) {
				memcpy(&j,s.data+(i+1)*sizeof(double),sizeof(double));
				t->push_back(j);
			}
		}
	};
}
// This is the trigger. In order to experiment with non-trigger version,
// I limited the maximum distance will be 20.

struct SSSPTrigger : public Trigger<int, double> {
	public:
		void Fire(const int* key, double* value, const double& newvalue, bool* doUpdate) {
			//fprintf(stderr,"TRIGGER: k=%d,v=%f,nv=%f\n",*key,*value,newvalue);
			if (*value <= newvalue) {
				doUpdate = false;
				return;
			}
			vector<double> thisnode = nodes->get(*key);
			vector<double>::iterator it = thisnode.begin();
			for(; it!=thisnode.end(); it++)
				distance_map->enqueue_update((*it), newvalue+1);
			*value = newvalue;
			*doUpdate = true;
			return;
		}
		bool LongFire(const int& key) {
			return false;
		}
};

static void BuildGraph(int shards, int nodes_record, int density) {
	vector<RecordFile*> out(shards);
	File::Mkdirs("testdata/");
	for (int i = 0; i < shards; ++i) {
		out[i] = new RecordFile(StringPrintf("testdata/sp-graph.rec-%05d-of-%05d", i, shards), "w");
	}

	fprintf(stderr, "Building graph: ");

	for (int i = 0; i < nodes_record; i++) {
		PathNode n;
		n.set_id(i);

		for (int j = 0; j < density; j++) {
			n.add_target(random() % nodes_record);
		}

		out[i % shards]->write(n);
		if (i % (nodes_record / 50) == 0) { fprintf(stderr, "."); }
	}
	fprintf(stderr, "\n");

	for (int i = 0; i < shards; ++i) {
		delete out[i];
	}
}

int ShortestPathTrigger(ConfigData& conf) {
	NUM_WORKERS = conf.num_workers();

	distance_map = CreateTable(0, FLAGS_shards, new Sharding::Mod, new Triggers<int,double>::NullTrigger);
	nodes_record = CreateRecordTable<PathNode>(1, "testdata/sp-graph.rec*", false);
	nodes        = CreateTable(2, FLAGS_shards, new Sharding::Mod, new Accumulators<vector<double> >::Replace);
	//TriggerID trigid = distance_map->register_trigger(new SSSPTrigger);

	StartWorker(conf);
	Master m(conf);

	if (FLAGS_build_graph) {
		BuildGraph(FLAGS_shards, FLAGS_tnum_nodes, 4);
		return 0;
	}

	PRunOne(distance_map, {
			for (int i = 0; i < FLAGS_tnum_nodes; ++i) {
				distance_map->update(i, 1e9);
			}

			});

	//Start all vectors with empty adjacency lists
	PRunOne(nodes, {
			vector<double> v;
			v.clear();

			nodes->resize(FLAGS_tnum_nodes);
			for(int i=0; i<FLAGS_tnum_nodes; i++)
			nodes->update(i,v);
			});

	//Build adjacency lists by appending RecordTables' contents
	PMap({n: nodes_record}, {
			vector<double> v=nodes->get(n.id());
			for(int i=0; i < n.target_size(); i++)
			v.push_back(n.target(i));
			//cout << "writing node " << n.id() << endl;
			nodes->update(n.id(),v);
			});

	PRunAll(distance_map, {
			distance_map->swap_accumulator((Trigger<int,double>*)new SSSPTrigger);
			});

	//Start the timer!
	struct timeval start_time, end_time;
	gettimeofday(&start_time, NULL);

	PRunOne(distance_map, {
			// Initialize a root node.
			// and enable the trigger.
			distance_map->update(0, 0);
			});

	PRunAll(distance_map, {
			distance_map->swap_accumulator(new Triggers<int,double>::NullTrigger);
			});


	//Finish the timer!
	gettimeofday(&end_time, NULL);
	long long totaltime = (long long) (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);
	cout << "Total SSSP time: " << ((double)(totaltime)/1000000.0) << " seconds" << endl;

	if (FLAGS_tdump_output) {
		PRunOne(distance_map, {
				for (int i = 0; i < FLAGS_tnum_nodes; ++i) {
				if (i % 30 == 0) {
				fprintf(stderr, "\n%5d: ", i);
				}

				int d = (int)distance_map->get(i);
				if (d >= 1000) { d = -1; }
				fprintf(stderr, "%3d ", d);
				}
				fprintf(stderr, "\n");
				});
	}
	return 0;
}
REGISTER_RUNNER(ShortestPathTrigger);
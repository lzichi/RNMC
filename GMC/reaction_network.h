#ifndef REACTION_NETWORK_H
#define REACTION_NETWORK_H

#include <stdint.h>
#include <vector>
#include <optional>
#include <mutex>
#include <memory>
#include <unordered_map>
#include "../core/sql.h"
#include "../GMC/sql_types.h"
#include "../core/simulation.h"

class Reaction {
    public:
    // we assume that each reaction has zero, one or two reactants
    uint8_t number_of_reactants;
    uint8_t number_of_products;

    int reactants[2];
    int products[2];

    double rate;
};

enum Phase {LATTICE, SOLUTION};
enum Type {ADSORPTION, DESORPTION, HOMOGENEOUS_ELYTE, HOMOGENEOUS_SOLID, DIFFUSION,CHARGE_TRANSFER};

class LatticeReaction : public Reaction {
    public:
    // we assume that each reaction has zero, one or two reactants
    uint8_t number_of_reactants;
    uint8_t number_of_products;

    int reactants[2];
    int products[2];

    Phase phase_reactants[2];
    Phase phase_products[2];

    double dG;
    double reorganization;
    double rate;

    Type type;
};

// parameters passed to the ReactionNetwork constructor
// by the dispatcher which are model specific
struct ReactionNetworkParameters {
};


class ReactionNetwork {
    public:
    std::vector<std::shared_ptr<Reaction>> reactions; // list of reactions (either Reaction or LatticeReactions)
    std::vector<int> initial_state; // initial state for all the simulations
    std::vector<double> initial_propensities; // initial propensities for all the reactions
    double factor_zero; // rate modifer for reactions with zero reactants
    double factor_two; // rate modifier for reactions with two reactants
    double factor_duplicate; // rate modifier for reactions of form A + A -> ...

    // maps species to the reactions which involve that species
    std::vector<std::vector<int>> dependents;

    ReactionNetwork(
        SqlConnection &reaction_network_database,
        SqlConnection &initial_state_database,
        ReactionNetworkParameters parameters);

    virtual void compute_dependents();

    virtual double compute_propensity(std::vector<int> &state,
        int reaction_index);

    void update_state(std::vector<int> &state,
        int reaction_index);

    virtual void update_propensities(
        std::function<void(Update update)> update_function,
        std::vector<int> &state,
        int next_reaction
    );

    // convert a history element as found a simulation to history
    // to a SQL type.
    TrajectoriesSql history_element_to_sql(
        int seed,
        HistoryElement history_element);

    // overwritten by base class
    virtual void init(SqlConnection &reaction_network_database);
    virtual void fill_reactions(SqlConnection &reaction_network_database);
};

ReactionNetwork::ReactionNetwork(
     SqlConnection &reaction_network_database,
     SqlConnection &initial_state_database,
     ReactionNetworkParameters)

    {

    // collecting reaction network metadata
    SqlStatement<MetadataSql> metadata_statement (reaction_network_database);
    SqlReader<MetadataSql> metadata_reader (metadata_statement);

    std::optional<MetadataSql> maybe_metadata_row = metadata_reader.next();

    if (! maybe_metadata_row.has_value()) {
        std::cerr << time_stamp()
                  << "no metadata row\n";

        std::abort();
    }

    MetadataSql metadata_row = maybe_metadata_row.value();

    // setting reaction network factors
    SqlStatement<FactorsSql> factors_statement (initial_state_database);
    SqlReader<FactorsSql> factors_reader (factors_statement);

    // TODO: make sure this isn't nothing
    FactorsSql factors_row = factors_reader.next().value();
    factor_zero = factors_row.factor_zero;
    factor_two = factors_row.factor_two;
    factor_duplicate = factors_row.factor_duplicate;

    // loading intial state
    initial_state.resize(metadata_row.number_of_species);

    SqlStatement<InitialStateSql> initial_state_statement (initial_state_database);
    SqlReader<InitialStateSql> initial_state_reader (initial_state_statement);

    int species_id;
    while(std::optional<InitialStateSql> maybe_initial_state_row =
          initial_state_reader.next()) {

        InitialStateSql initial_state_row = maybe_initial_state_row.value();
        species_id = initial_state_row.species_id;
        initial_state[species_id] = initial_state_row.count;
    }
    reactions.reserve(metadata_row.number_of_reactions);
    initial_propensities.resize(metadata_row.number_of_reactions);

};

void ReactionNetwork::init(SqlConnection &reaction_network_database) {
    // loading reactions
    // vectors are default initialized to empty.
    // it is "cleaner" to resize the default vector than to
    // drop it and reinitialize a new vector.

    fill_reactions(reaction_network_database);

    // computing initial propensities
    for (unsigned long int i = 0; i < initial_propensities.size(); i++) {
        initial_propensities[i] = compute_propensity(initial_state, i);
    }

    std::cerr << time_stamp() << "computing dependency graph...\n";
    compute_dependents();
    std::cerr << time_stamp() << "finished computing dependency graph\n";
}

void ReactionNetwork::compute_dependents() {
    // initializing dependency graph

    dependents.resize(initial_state.size());


    for ( unsigned int reaction_id = 0; reaction_id <  reactions.size(); reaction_id++ ) {
        std::shared_ptr<Reaction> reaction = reactions[reaction_id];

        for ( int i = 0; i < reaction->number_of_reactants; i++ ) {
            int reactant_id = reaction->reactants[i];
            dependents[reactant_id].push_back(reaction_id);
        }
    }

};


double ReactionNetwork::compute_propensity(std::vector<int> &state,
    int reaction_index) {

    std::shared_ptr<Reaction> reaction = reactions[reaction_index];

    double p;
    // zero reactants
    if (reaction->number_of_reactants == 0)
        p = factor_zero * reaction->rate;

    // one reactant
    else if (reaction->number_of_reactants == 1)
        p = state[reaction->reactants[0]] * reaction->rate;


    // two reactants
    else {
        if (reaction->reactants[0] == reaction->reactants[1])
            p = factor_duplicate
                * factor_two
                * state[reaction->reactants[0]]
                * (state[reaction->reactants[0]] - 1)
                * reaction->rate;

        else
            p = factor_two
                * state[reaction->reactants[0]]
                * state[reaction->reactants[1]]
                * reaction->rate;
    }

    return p;

};

void ReactionNetwork::update_state(std::vector<int> &state,
    int reaction_index) {

    for (int m = 0;
         m < reactions[reaction_index]->number_of_products;
         m++) {
        state[reactions[reaction_index]->reactants[m]]--;
    }

    for (int m = 0;
         m < reactions[reaction_index]->number_of_products;
         m++) {
        state[reactions[reaction_index]->products[m]]++;
    }

}


void ReactionNetwork::update_propensities(
    std::function<void(Update update)> update_function,
    std::vector<int> &state,
    int next_reaction
    ) {

    std::shared_ptr<Reaction> reaction = reactions[next_reaction];

    std::vector<int> species_of_interest;
    species_of_interest.reserve(4);

    for ( int i = 0; i < reaction->number_of_reactants; i++ ) {
        int reactant_id = reaction->reactants[i];
        species_of_interest.push_back(reactant_id);
    }


    for ( int j = 0; j < reaction->number_of_products; j++ ) {
        int product_id = reaction->products[j];
        species_of_interest.push_back(product_id);
    }


    for ( int species_id : species_of_interest ) {
        for ( unsigned int reaction_index : dependents[species_id] ) {

            double new_propensity = compute_propensity(state,
                reaction_index);

            update_function(Update {
                    .index = reaction_index,
                    .propensity = new_propensity});
        }
    }
}

void ReactionNetwork::fill_reactions(SqlConnection &reaction_network_database) {

    SqlStatement<ReactionSql> reaction_statement (reaction_network_database);
    SqlReader<ReactionSql> reaction_reader (reaction_statement);


    // reaction_id is lifted so we can do a sanity check after the
    // loop.  Make sure size of reactions vector, last reaction_id and
    // metadata number_of_reactions are all the same

    // read in Reactions
    while(std::optional<ReactionSql> maybe_reaction_row = reaction_reader.next()) {

        ReactionSql reaction_row = maybe_reaction_row.value();
        uint8_t number_of_reactants = reaction_row.number_of_reactants;
        uint8_t number_of_products = reaction_row.number_of_products;

        std::shared_ptr<Reaction> reaction = std::make_shared<Reaction>();
        reaction->number_of_reactants = number_of_reactants;
        reaction->number_of_products = number_of_products;
        reaction->reactants[0] = reaction_row.reactant_1;
        reaction->reactants[1] = reaction_row.reactant_2;
        reaction->products [0]= reaction_row.product_1;
        reaction->products[1] = reaction_row.product_2;
        reaction->rate = reaction_row.rate;

        reactions.push_back(reaction);
    }

}

TrajectoriesSql ReactionNetwork::history_element_to_sql(
    int seed,
    HistoryElement history_element) {
    return TrajectoriesSql {
        .seed = seed,
        .step = history_element.step,
        .reaction_id = history_element.reaction_id,
        .time = history_element.time
    };
}


class LatticeReactionNetwork : public ReactionNetwork{
    public:
    //LatticeReactionNetwork(std::vector<std::pair<int, int>>  &lattice_sites_in, int empty);
    void init(SqlConnection &reaction_network_database);
    void fill_reactions(SqlConnection &reaction_network_database);
    void compute_dependents();
    void increase_species(int species_id);
    void decrease_species(int species_id);
    void update_propensities(std::function<void(Update update)> update_function,
                             int next_reaction);
    //std::unordered_map<int, std::vector<int> > lattice_sites; // key: species type, value: vector of sites
    //int empty_lattice_sites;

};

/*LatticeReactionNetwork::LatticeReactionNetwork(std::vector<std::pair<int, int>>  &lattice_sites_in, int empty) {
    empty_lattice_sites = empty;
    // TODO make sure this is the right way to initalize the vector 
    lattice_sites = lattice_sites_in;
}*/

void LatticeReactionNetwork::fill_reactions(SqlConnection &reaction_network_database) {
    
    SqlStatement<LatticeReactionSql> reaction_statement (reaction_network_database);
    SqlReader<LatticeReactionSql> reaction_reader (reaction_statement);

    // reaction_id is lifted so we can do a sanity check after the
    // loop.  Make sure size of reactions vector, last reaction_id and
    // metadata number_of_reactions are all the same

    // read in Reactions
    while(std::optional<LatticeReactionSql> maybe_reaction_row = reaction_reader.next()) {

        LatticeReactionSql reaction_row = maybe_reaction_row.value();

        std::shared_ptr<LatticeReaction> reaction = std::make_shared<LatticeReaction>();

        reaction->number_of_reactants = static_cast<uint8_t> (reaction_row.number_of_reactants);
        reaction->number_of_products = static_cast<uint8_t> (reaction_row.number_of_products);
        reaction->reactants[0] = reaction_row.reactant_1;
        reaction->reactants[1] = reaction_row.reactant_2;
        reaction->products [0]= reaction_row.product_1;
        reaction->products[1] = reaction_row.product_2;
        reaction->rate = reaction_row.rate;
        reaction->dG = reaction_row.dG;
        reaction->reorganization = reaction_row.reorganization;

        reaction->phase_reactants[0] = (reaction_row.phase_reactant_1 == 'L') ? Phase::LATTICE : Phase::SOLUTION;
        reaction->phase_reactants[1] = (reaction_row.phase_reactant_2 == 'L') ? Phase::LATTICE : Phase::SOLUTION;
        reaction->phase_products[0] = (reaction_row.phase_product_1 == 'L') ? Phase::LATTICE : Phase::SOLUTION;
        reaction->phase_products[1] = (reaction_row.phase_product_2 == 'L') ? Phase::LATTICE : Phase::SOLUTION;

        if(reaction_row.type == 'E') {
            reaction->type = Type::ELECTROCHEMICAL;
        }
        else if (reaction_row.type == 'D') {
            reaction->type = Type::DIFFUSION;
        }
        else if (reaction_row.type == 'C') {
            reaction->type = Type::CHEMICAL;
        }
        else if (reaction_row.type == 'A') {
            reaction->type = Type::ADSORPTION;
        }

        reactions.push_back(reaction);
    }
}

void LatticeReactionNetwork::init(SqlConnection &reaction_network_database) {
    // loading reactions
    // vectors are default initialized to empty.
    // it is "cleaner" to resize the default vector than to
    // drop it and reinitialize a new vector.

    fill_reactions(reaction_network_database);

    // computing initial propensities
    for (unsigned long int i = 0; i < initial_propensities.size(); i++) {
        initial_propensities[i] = compute_propensity(i);
    }

    std::cerr << time_stamp() << "computing dependency graph...\n";
    compute_dependents();
    std::cerr << time_stamp() << "finished computing dependency graph\n";
}

void LatticeReactionNetwork::compute_dependents() {
    // initializing dependency graph

    dependents.resize(state.size());


    for ( unsigned int reaction_id = 0; reaction_id <  reactions.size(); reaction_id++ ) {
        LatticeReaction *reaction = static_cast<LatticeReaction*> (reactions[reaction_id].get());

        for ( int i = 0; i < reaction->number_of_reactants; i++ ) {
            int reactant_id = reaction->reactants[i];
            dependents[reactant_id].push_back(reaction_id);
        }
    }

};

void LatticeReactionNetwork::increase_species(int species_id) {
    state[species_id++];
}

void LatticeReactionNetwork::decrease_species(int species_id) {
    state[species_id--];
}


void LatticeReactionNetwork::update_propensities(
    std::function<void(Update update)> update_function,
    int next_reaction
    ) {

    LatticeReaction *reaction = static_cast<LatticeReaction *> (reactions[next_reaction].get());

    std::vector<int> species_of_interest;
    species_of_interest.reserve(4);

    for ( int i = 0; i < reaction->number_of_reactants; i++ ) {
        int reactant_id = reaction->reactants[i];
        species_of_interest.push_back(reactant_id);
    }


    for ( int j = 0; j < reaction->number_of_products; j++ ) {
        // make sure product is in the solution
        if(reaction->phase_products[j] == Phase::SOLUTION) {
            int product_id = reaction->products[j];
            species_of_interest.push_back(product_id);
        }
        
    }


    for ( int species_id : species_of_interest ) {
        for ( unsigned int reaction_index : dependents[species_id] ) {

            double new_propensity = compute_propensity(
                reaction_index);

            update_function(Update {
                    .index = reaction_index,
                    .propensity = new_propensity});
        }
    }
}
#endif